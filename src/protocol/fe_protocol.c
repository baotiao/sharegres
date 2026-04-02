#include "fe_protocol.h"
#include "message.h"
#include "../log.h"
#include "../parser/query_parser.h"
#include "../router/router.h"
#include "../executor/copy_handler.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

/*
 * Connect to a backend (either pool or fallback).
 * For single-shard route, uses the shard's pool.
 * For default route, uses the fallback conninfo.
 */
static int connect_to_shard(ClientSession *session, Shard *shard)
{
    PGconn *conn = conn_pool_acquire(&shard->pool);
    if (!conn) {
        log_error("failed to acquire connection for shard '%s'", shard->name);
        return -1;
    }

    PQsetnonblocking(conn, 1);

    session->backend_conn = conn;
    session->backend_shard = shard;
    session->backend_ev.fd = PQsocket(conn);
    session->backend_ev.data = session;
    session->backend_ev.callback = session_on_backend_readable;
    event_loop_add(session->loop, &session->backend_ev, EPOLLIN);

    return 0;
}

static int connect_to_default(ClientSession *session)
{
    if (session->backend_conn) {
        ConnStatusType st = PQstatus(session->backend_conn);
        if (st == CONNECTION_OK)
            return 0;
        event_loop_del(session->loop, &session->backend_ev);
        if (session->backend_shard) {
            conn_pool_release(&session->backend_shard->pool, session->backend_conn);
        } else {
            PQfinish(session->backend_conn);
        }
        session->backend_conn = NULL;
        session->backend_shard = NULL;
    }

    log_debug("connecting to default backend: %s", session->backend_conninfo);
    session->backend_conn = PQconnectdb(session->backend_conninfo);

    if (PQstatus(session->backend_conn) != CONNECTION_OK) {
        log_error("default backend connect failed: %s",
                  PQerrorMessage(session->backend_conn));
        PQfinish(session->backend_conn);
        session->backend_conn = NULL;
        return -1;
    }

    PQsetnonblocking(session->backend_conn, 1);
    session->backend_shard = NULL;
    int be_fd = PQsocket(session->backend_conn);
    log_debug("default backend fd=%d", be_fd);
    session->backend_ev.fd = be_fd;
    session->backend_ev.data = session;
    session->backend_ev.callback = session_on_backend_readable;
    event_loop_add(session->loop, &session->backend_ev, EPOLLIN);

    return 0;
}

/*
 * Execute a query on a single shard/backend.
 */
static int execute_single(ClientSession *session, const char *sql, Shard *shard)
{
    /* Release any existing connection if it's to a different shard */
    if (session->backend_conn && session->backend_shard != shard) {
        event_loop_del(session->loop, &session->backend_ev);
        if (session->backend_shard) {
            conn_pool_release(&session->backend_shard->pool, session->backend_conn);
        } else {
            PQfinish(session->backend_conn);
        }
        session->backend_conn = NULL;
        session->backend_shard = NULL;
    }

    if (!session->backend_conn) {
        if (shard) {
            if (connect_to_shard(session, shard) < 0)
                return -1;
        } else {
            if (connect_to_default(session) < 0)
                return -1;
        }
    }

    if (PQsendQuery(session->backend_conn, sql) == 0) {
        log_error("PQsendQuery failed: %s",
                  PQerrorMessage(session->backend_conn));
        return -1;
    }

    PQflush(session->backend_conn);
    session->state = SESSION_STATE_QUERY;
    session->backend_busy = true;
    return 0;
}

/*
 * Execute scatter-gather: send query to all target shards.
 */
static int execute_scatter(ClientSession *session, const char *sql,
                           Shard **shards, int count)
{
    if (count > MAX_SCATTER_SHARDS) {
        log_error("too many scatter shards: %d > %d", count, MAX_SCATTER_SHARDS);
        return -1;
    }

    session->scatter_count = count;
    session->scatter_done = 0;
    session->scatter_header_sent = false;

    for (int i = 0; i < count; i++) {
        ScatterConn *sc = &session->scatter[i];
        memset(sc, 0, sizeof(ScatterConn));
        sc->shard = shards[i];
        sc->session = session;

        sc->conn = conn_pool_acquire(&shards[i]->pool);
        if (!sc->conn) {
            log_error("scatter: failed to acquire conn for shard '%s'",
                      shards[i]->name);
            /* Mark as done (failed) */
            sc->done = true;
            session->scatter_done++;
            continue;
        }

        sc->from_pool = true;
        PQsetnonblocking(sc->conn, 1);

        if (PQsendQuery(sc->conn, sql) == 0) {
            log_error("scatter: PQsendQuery failed for shard '%s': %s",
                      shards[i]->name, PQerrorMessage(sc->conn));
            conn_pool_release(&shards[i]->pool, sc->conn);
            sc->conn = NULL;
            sc->done = true;
            session->scatter_done++;
            continue;
        }

        PQflush(sc->conn);

        /* Register with epoll */
        sc->ev.fd = PQsocket(sc->conn);
        sc->ev.data = sc;  /* store ScatterConn pointer */
        sc->ev.callback = session_on_scatter_readable;
        event_loop_add(session->loop, &sc->ev, EPOLLIN);
    }

    /* If all failed immediately */
    if (session->scatter_done >= count) {
        fe_send_error(session, "ERROR", "08001",
                      "all scatter connections failed");
        MsgBuf msg;
        msgbuf_init(&msg, 16);
        msg_write_ready_for_query(&msg, TXN_IDLE);
        session_queue_send(session, msg.data, msg.len);
        msgbuf_free(&msg);
        session->state = SESSION_STATE_READY;
        return 0;
    }

    session->state = SESSION_STATE_SCATTER;
    return 0;
}

void fe_send_startup_response(ClientSession *session)
{
    MsgBuf *buf = &session->send_buf;

    msg_write_auth_ok(buf);
    msg_write_param_status(buf, "server_version", "16.0 (Sharegres)");
    msg_write_param_status(buf, "server_encoding", "UTF8");
    msg_write_param_status(buf, "client_encoding", "UTF8");
    msg_write_param_status(buf, "is_superuser", "off");
    msg_write_param_status(buf, "DateStyle", "ISO, MDY");
    msg_write_param_status(buf, "integer_datetimes", "on");
    msg_write_param_status(buf, "standard_conforming_strings", "on");
    msg_write_backend_key(buf, session->pid, session->secret_key);
    msg_write_ready_for_query(buf, TXN_IDLE);
}

void fe_send_error(ClientSession *session, const char *severity,
                   const char *code, const char *message)
{
    msg_write_error(&session->send_buf, severity, code, message);
}

static int handle_startup_message(ClientSession *session, size_t msg_len)
{
    RecvBuf *rb = &session->recv_buf;

    if (msg_len < 8)
        return -1;

    uint32_t version;
    memcpy(&version, rb->data + 4, 4);
    version = ntohl(version);

    if (version == PG_SSL_REQUEST) {
        uint8_t no_ssl = 'N';
        session_queue_send(session, &no_ssl, 1);
        memmove(rb->data, rb->data + msg_len, rb->len - msg_len);
        rb->len -= msg_len;
        return 0;
    }

    if (version == PG_CANCEL_REQUEST) {
        log_info("cancel request received (not implemented)");
        memmove(rb->data, rb->data + msg_len, rb->len - msg_len);
        rb->len -= msg_len;
        return -1;
    }

    int major = version >> 16;
    if (major != PG_PROTOCOL_MAJOR) {
        log_error("unsupported protocol version %d.%d", major, version & 0xFFFF);
        return -1;
    }

    log_info("startup: protocol %d.%d", major, version & 0xFFFF);

    size_t pos = 8;
    while (pos < msg_len) {
        if (rb->data[pos] == '\0')
            break;

        const char *key = (const char *)(rb->data + pos);
        pos += strlen(key) + 1;
        if (pos >= msg_len)
            break;

        const char *val = (const char *)(rb->data + pos);
        pos += strlen(val) + 1;

        log_debug("startup param: %s=%s", key, val);

        if (strcmp(key, "user") == 0) {
            free(session->user);
            session->user = strdup(val);
        } else if (strcmp(key, "database") == 0) {
            free(session->database);
            session->database = strdup(val);
        }
    }

    if (!session->user)
        session->user = strdup("unknown");
    if (!session->database)
        session->database = strdup(session->user);

    log_info("client login: user=%s database=%s", session->user, session->database);

    memmove(rb->data, rb->data + msg_len, rb->len - msg_len);
    rb->len -= msg_len;

    fe_send_startup_response(session);
    session->state = SESSION_STATE_READY;
    return 0;
}

/*
 * Handle a Query ('Q') message with routing.
 */
static int handle_query(ClientSession *session, const char *sql)
{
    log_info("query from fd=%d: %.128s", session->client_fd, sql);

    /* If no shard map configured, use default backend (Phase 1 behavior) */
    if (!session->shard_map || session->shard_map->shard_count == 0) {
        if (connect_to_default(session) < 0) {
            fe_send_error(session, "FATAL", "08001", "cannot connect to backend");
            MsgBuf msg;
            msgbuf_init(&msg, 16);
            msg_write_ready_for_query(&msg, TXN_IDLE);
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
            return 0;
        }

        if (PQsendQuery(session->backend_conn, sql) == 0) {
            fe_send_error(session, "ERROR", "XX000",
                          PQerrorMessage(session->backend_conn));
            MsgBuf msg;
            msgbuf_init(&msg, 16);
            msg_write_ready_for_query(&msg, TXN_IDLE);
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
            return 0;
        }
        PQflush(session->backend_conn);
        session->state = SESSION_STATE_QUERY;
        session->backend_busy = true;
        return 0;
    }

    /* Parse query and determine routing */
    ParseResult parsed;
    if (parse_query(sql, &parsed) < 0) {
        fe_send_error(session, "ERROR", "42601", "syntax error in query");
        MsgBuf msg;
        msgbuf_init(&msg, 16);
        msg_write_ready_for_query(&msg, TXN_IDLE);
        session_queue_send(session, msg.data, msg.len);
        msgbuf_free(&msg);
        return 0;
    }

    RouteDecision decision;
    router_decide(session->shard_map, &parsed, &decision);

    int rc = 0;

    switch (decision.type) {
    case ROUTE_SINGLE:
        log_info("route: SINGLE -> shard '%s'",
                 decision.target_shards[0]->name);
        rc = execute_single(session, sql, decision.target_shards[0]);
        if (rc < 0) {
            fe_send_error(session, "ERROR", "08001",
                          "cannot connect to shard");
            MsgBuf msg;
            msgbuf_init(&msg, 16);
            msg_write_ready_for_query(&msg, TXN_IDLE);
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
            rc = 0;
        }
        break;

    case ROUTE_SCATTER:
        log_info("route: SCATTER -> %d shards", decision.target_count);
        rc = execute_scatter(session, sql,
                             decision.target_shards, decision.target_count);
        break;

    case ROUTE_LOCAL:
        log_info("route: LOCAL (handled by proxy)");
        /* For now, just return OK for SET/SHOW */
        {
            MsgBuf msg;
            msgbuf_init(&msg, 64);
            msg_write_command_complete(&msg, "SET");
            session_queue_send(session, msg.data, msg.len);
            msgbuf_reset(&msg);
            msg_write_ready_for_query(&msg, TXN_IDLE);
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
        }
        break;

    case ROUTE_DEFAULT:
        log_info("route: DEFAULT -> fallback backend");
        rc = execute_single(session, sql, NULL);
        if (rc < 0) {
            fe_send_error(session, "ERROR", "08001",
                          "cannot connect to default backend");
            MsgBuf msg;
            msgbuf_init(&msg, 16);
            msg_write_ready_for_query(&msg, TXN_IDLE);
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
            rc = 0;
        }
        break;
    }

    parse_result_free(&parsed);
    return rc;
}

int fe_protocol_process(ClientSession *session)
{
    RecvBuf *rb = &session->recv_buf;

    while (rb->len > 0) {
        if (session->state == SESSION_STATE_INIT) {
            if (rb->len < 4)
                return 0;

            uint32_t msg_len;
            memcpy(&msg_len, rb->data, 4);
            msg_len = ntohl(msg_len);

            if (msg_len < 4 || msg_len > 65535) {
                log_error("invalid startup message length: %u", msg_len);
                return -1;
            }

            if (rb->len < msg_len)
                return 0;

            if (handle_startup_message(session, msg_len) < 0)
                return -1;
            continue;
        }

        /* If we're busy waiting for backend (non-COPY), don't process more */
        if (session->state == SESSION_STATE_QUERY ||
            session->state == SESSION_STATE_SCATTER ||
            session->state == SESSION_STATE_COPY_OUT) {
            return 0;
        }

        if (rb->len < 5)
            return 0;

        uint8_t msg_type = rb->data[0];
        uint32_t msg_len;
        memcpy(&msg_len, rb->data + 1, 4);
        msg_len = ntohl(msg_len);

        size_t total = 1 + msg_len;

        if (msg_len < 4 || total > 16 * 1024 * 1024) {
            log_error("invalid message: type=%c len=%u", msg_type, msg_len);
            return -1;
        }

        if (rb->len < total)
            return 0;

        /* COPY IN mode: forward CopyData/CopyDone/CopyFail to backend */
        if (session->state == SESSION_STATE_COPY_IN) {
            if (msg_type == MSG_COPY_DATA || msg_type == MSG_COPY_DONE ||
                msg_type == MSG_COPY_FAIL) {
                /* Payload is after type(1) + length(4), length is msg_len-4 */
                const uint8_t *payload = rb->data + 5;
                size_t payload_len = msg_len - 4;
                int rc = copy_handle_client_message(session, msg_type,
                                                    payload, payload_len);
                memmove(rb->data, rb->data + total, rb->len - total);
                rb->len -= total;

                if (rc < 0)
                    return -1;
                if (rc == 1) {
                    /* COPY finished, switch back to QUERY to wait for
                     * backend CommandComplete */
                    session->state = SESSION_STATE_QUERY;
                }
                continue;
            }
            /* Unexpected message during COPY IN */
            log_warn("unexpected message type 0x%02x during COPY IN", msg_type);
            memmove(rb->data, rb->data + total, rb->len - total);
            rb->len -= total;
            continue;
        }

        switch (msg_type) {
        case MSG_QUERY: {
            const char *query = (const char *)(rb->data + 5);
            memmove(rb->data, rb->data + total, rb->len - total);
            rb->len -= total;
            return handle_query(session, query);
        }

        case MSG_TERMINATE:
            log_info("client fd=%d sent Terminate", session->client_fd);
            return -1;

        case MSG_PARSE:
        case MSG_BIND:
        case MSG_DESCRIBE:
        case MSG_EXECUTE:
        case MSG_SYNC:
        case MSG_CLOSE:
            log_warn("extended query protocol not yet supported (type=%c)", msg_type);
            fe_send_error(session, "ERROR", "0A000",
                          "extended query protocol not yet supported");
            {
                MsgBuf msg;
                msgbuf_init(&msg, 16);
                msg_write_ready_for_query(&msg, TXN_IDLE);
                session_queue_send(session, msg.data, msg.len);
                msgbuf_free(&msg);
            }
            break;

        default:
            log_warn("unknown message type: %c (0x%02x)", msg_type, msg_type);
            break;
        }

        memmove(rb->data, rb->data + total, rb->len - total);
        rb->len -= total;
    }

    return 0;
}
