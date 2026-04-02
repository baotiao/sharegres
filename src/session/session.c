#include "session.h"
#include "../protocol/fe_protocol.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define RECV_BUF_INITIAL    8192
#define SEND_BUF_INITIAL    8192

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void recvbuf_init(RecvBuf *rb, size_t cap)
{
    rb->data = malloc(cap);
    rb->len = 0;
    rb->cap = cap;
}

void recvbuf_free(RecvBuf *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static void recvbuf_ensure(RecvBuf *rb, size_t additional)
{
    if (rb->len + additional <= rb->cap)
        return;
    size_t new_cap = rb->cap * 2;
    while (new_cap < rb->len + additional)
        new_cap *= 2;
    rb->data = realloc(rb->data, new_cap);
    rb->cap = new_cap;
}

ClientSession* session_create(int client_fd, EventLoop *loop,
                              const char *backend_conninfo, ShardMap *shard_map)
{
    ClientSession *s = calloc(1, sizeof(ClientSession));
    if (!s)
        return NULL;

    s->client_fd = client_fd;
    s->state = SESSION_STATE_INIT;
    s->txn_state = TXN_STATE_IDLE;
    s->loop = loop;
    s->backend_conninfo = backend_conninfo;
    s->shard_map = shard_map;

    set_nonblocking(client_fd);

    recvbuf_init(&s->recv_buf, RECV_BUF_INITIAL);
    msgbuf_init(&s->send_buf, SEND_BUF_INITIAL);
    s->send_pos = 0;

    s->ev_handler.fd = client_fd;
    s->ev_handler.data = s;
    s->ev_handler.callback = session_on_client_readable;

    event_loop_add(loop, &s->ev_handler, EPOLLIN);

    s->pid = getpid();
    s->secret_key = (int32_t)((uintptr_t)s & 0x7FFFFFFF);

    log_info("new client session fd=%d", client_fd);
    return s;
}

static void session_release_scatter(ClientSession *session)
{
    for (int i = 0; i < session->scatter_count; i++) {
        ScatterConn *sc = &session->scatter[i];
        if (sc->conn) {
            event_loop_del(session->loop, &sc->ev);
            if (sc->from_pool && sc->shard) {
                conn_pool_release(&sc->shard->pool, sc->conn);
            } else {
                PQfinish(sc->conn);
            }
            sc->conn = NULL;
        }
    }
    session->scatter_count = 0;
    session->scatter_done = 0;
    session->scatter_header_sent = false;
}

void session_destroy(ClientSession *session)
{
    log_info("destroying session fd=%d", session->client_fd);

    event_loop_del(session->loop, &session->ev_handler);
    close(session->client_fd);

    /* Release single-shard backend */
    if (session->backend_conn) {
        event_loop_del(session->loop, &session->backend_ev);
        if (session->backend_shard) {
            conn_pool_release(&session->backend_shard->pool, session->backend_conn);
        } else {
            PQfinish(session->backend_conn);
        }
        session->backend_conn = NULL;
    }

    /* Release scatter connections */
    session_release_scatter(session);

    recvbuf_free(&session->recv_buf);
    msgbuf_free(&session->send_buf);
    free(session->user);
    free(session->database);
    free(session);
}

void session_queue_send(ClientSession *session, const void *data, size_t len)
{
    msgbuf_put_bytes(&session->send_buf, data, len);
}

int session_send(ClientSession *session)
{
    while (session->send_pos < session->send_buf.len) {
        ssize_t n = send(session->client_fd,
                         session->send_buf.data + session->send_pos,
                         session->send_buf.len - session->send_pos,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                session->ev_handler.callback = session_on_client_writable;
                event_loop_mod(session->loop, &session->ev_handler,
                               EPOLLOUT);
                return 0;
            }
            log_error("send to client fd=%d failed: %s",
                      session->client_fd, strerror(errno));
            return -1;
        }
        session->send_pos += n;
    }

    msgbuf_reset(&session->send_buf);
    session->send_pos = 0;

    session->ev_handler.callback = session_on_client_readable;
    event_loop_mod(session->loop, &session->ev_handler, EPOLLIN);
    return 0;
}

void session_on_client_writable(EventHandler *handler, uint32_t events)
{
    ClientSession *session = handler->data;
    if (events & (EPOLLERR | EPOLLHUP)) {
        session_destroy(session);
        return;
    }
    if (session_send(session) < 0)
        session_destroy(session);
}

void session_on_client_readable(EventHandler *handler, uint32_t events)
{
    ClientSession *session = handler->data;

    if (events & (EPOLLERR | EPOLLHUP)) {
        session_destroy(session);
        return;
    }

    recvbuf_ensure(&session->recv_buf, 4096);
    ssize_t n = recv(session->client_fd,
                     session->recv_buf.data + session->recv_buf.len,
                     session->recv_buf.cap - session->recv_buf.len,
                     0);
    if (n <= 0) {
        if (n == 0)
            log_info("client fd=%d disconnected", session->client_fd);
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
            log_error("recv from client fd=%d: %s",
                      session->client_fd, strerror(errno));
        else
            return;
        session_destroy(session);
        return;
    }
    session->recv_buf.len += n;

    if (fe_protocol_process(session) < 0) {
        session_destroy(session);
        return;
    }

    if (session->send_buf.len > session->send_pos) {
        if (session_send(session) < 0)
            session_destroy(session);
    }
}

/*
 * Helper: forward PGresult rows to client as wire protocol messages.
 * Returns number of rows forwarded.
 */
int session_forward_result(ClientSession *session, PGresult *res, bool send_header)
{
    int nfields = PQnfields(res);
    int nrows = PQntuples(res);
    MsgBuf msg;
    msgbuf_init(&msg, 256);

    if (send_header && nfields > 0) {
        /* RowDescription */
        msgbuf_put_byte(&msg, MSG_ROW_DESCRIPTION);
        size_t len_off = msg.len;
        msgbuf_put_int32(&msg, 0);
        msgbuf_put_int16(&msg, (int16_t)nfields);

        for (int c = 0; c < nfields; c++) {
            msgbuf_put_string(&msg, PQfname(res, c));
            msgbuf_put_int32(&msg, 0);
            msgbuf_put_int16(&msg, 0);
            msgbuf_put_int32(&msg, (int32_t)PQftype(res, c));
            msgbuf_put_int16(&msg, (int16_t)PQfsize(res, c));
            msgbuf_put_int32(&msg, (int32_t)PQfmod(res, c));
            msgbuf_put_int16(&msg, 0);
        }

        int32_t total_len = (int32_t)(msg.len - len_off);
        int32_t nlen = htonl(total_len);
        memcpy(msg.data + len_off, &nlen, 4);
        session_queue_send(session, msg.data, msg.len);
    }

    /* DataRows */
    for (int r = 0; r < nrows; r++) {
        msgbuf_reset(&msg);
        msgbuf_put_byte(&msg, MSG_DATA_ROW);
        size_t row_len_off = msg.len;
        msgbuf_put_int32(&msg, 0);
        msgbuf_put_int16(&msg, (int16_t)nfields);

        for (int c = 0; c < nfields; c++) {
            if (PQgetisnull(res, r, c)) {
                msgbuf_put_int32(&msg, -1);
            } else {
                int vlen = PQgetlength(res, r, c);
                msgbuf_put_int32(&msg, vlen);
                msgbuf_put_bytes(&msg, PQgetvalue(res, r, c), vlen);
            }
        }

        int32_t row_total = (int32_t)(msg.len - row_len_off);
        int32_t nrow_total = htonl(row_total);
        memcpy(msg.data + row_len_off, &nrow_total, 4);
        session_queue_send(session, msg.data, msg.len);
    }

    msgbuf_free(&msg);
    return nrows;
}

/*
 * Backend readable: single-shard query result.
 */
void session_on_backend_readable(EventHandler *handler, uint32_t events)
{
    ClientSession *session = handler->data;

    if (events & (EPOLLERR | EPOLLHUP)) {
        log_error("backend connection error for fd=%d", session->client_fd);
        fe_send_error(session, "FATAL", "08006", "backend connection lost");
        session_send(session);
        session_destroy(session);
        return;
    }

    if (!session->backend_conn)
        return;

    if (PQconsumeInput(session->backend_conn) == 0) {
        log_error("PQconsumeInput failed: %s",
                  PQerrorMessage(session->backend_conn));
        fe_send_error(session, "FATAL", "08006", "backend connection error");
        session_send(session);
        session_destroy(session);
        return;
    }

    if (PQisBusy(session->backend_conn))
        return;

    PGresult *res;
    int total_rows = 0;
    bool header_sent = false;

    while ((res = PQgetResult(session->backend_conn)) != NULL) {
        ExecStatusType status = PQresultStatus(res);

        switch (status) {
        case PGRES_TUPLES_OK:
        case PGRES_SINGLE_TUPLE: {
            int rows = session_forward_result(session, res, !header_sent);
            total_rows += rows;
            header_sent = true;
            /* Send CommandComplete */
            if (status == PGRES_TUPLES_OK) {
                const char *cmd_status = PQcmdStatus(res);
                char tag[128];
                if (cmd_status && cmd_status[0] != '\0') {
                    snprintf(tag, sizeof(tag), "%s", cmd_status);
                } else {
                    snprintf(tag, sizeof(tag), "SELECT %d", rows);
                }
                MsgBuf cmsg;
                msgbuf_init(&cmsg, 64);
                msg_write_command_complete(&cmsg, tag);
                session_queue_send(session, cmsg.data, cmsg.len);
                msgbuf_free(&cmsg);
            }
            break;
        }
        case PGRES_COMMAND_OK: {
            MsgBuf msg;
            msgbuf_init(&msg, 64);
            msg_write_command_complete(&msg, PQcmdStatus(res));
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
            break;
        }
        case PGRES_COPY_IN: {
            /* Backend wants COPY IN data - send CopyInResponse to client */
            int nfields = PQnfields(res);
            MsgBuf cmsg;
            msgbuf_init(&cmsg, 32);
            msgbuf_put_byte(&cmsg, MSG_COPY_IN_RESP);
            size_t coff = cmsg.len;
            msgbuf_put_int32(&cmsg, 0);
            msgbuf_put_byte(&cmsg, 0); /* text format */
            msgbuf_put_int16(&cmsg, (int16_t)nfields);
            for (int c = 0; c < nfields; c++)
                msgbuf_put_int16(&cmsg, 0); /* text format per column */
            int32_t clen = (int32_t)(cmsg.len - coff);
            int32_t nclen = htonl(clen);
            memcpy(cmsg.data + coff, &nclen, 4);
            session_queue_send(session, cmsg.data, cmsg.len);
            msgbuf_free(&cmsg);

            session->state = SESSION_STATE_COPY_IN;
            PQclear(res);
            log_info("COPY IN started, forwarding to client");
            /* Don't process more results - wait for client COPY data */
            goto flush_and_return;
        }

        case PGRES_COPY_OUT: {
            /* Backend is sending COPY OUT data */
            int nfields = PQnfields(res);
            MsgBuf cmsg;
            msgbuf_init(&cmsg, 32);
            msgbuf_put_byte(&cmsg, MSG_COPY_OUT_RESP);
            size_t coff = cmsg.len;
            msgbuf_put_int32(&cmsg, 0);
            msgbuf_put_byte(&cmsg, 0);
            msgbuf_put_int16(&cmsg, (int16_t)nfields);
            for (int c = 0; c < nfields; c++)
                msgbuf_put_int16(&cmsg, 0);
            int32_t clen = (int32_t)(cmsg.len - coff);
            int32_t nclen = htonl(clen);
            memcpy(cmsg.data + coff, &nclen, 4);
            session_queue_send(session, cmsg.data, cmsg.len);
            msgbuf_free(&cmsg);

            session->state = SESSION_STATE_COPY_OUT;
            PQclear(res);
            log_info("COPY OUT started");
            /* Fall through to read COPY OUT data below */
            goto copy_out_read;
        }

        case PGRES_FATAL_ERROR: {
            const char *severity = PQresultErrorField(res, PG_DIAG_SEVERITY);
            const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
            const char *primary  = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
            const char *detail   = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);

            /* Build a proper ErrorResponse with individual fields */
            MsgBuf emsg;
            msgbuf_init(&emsg, 256);
            msgbuf_put_byte(&emsg, MSG_ERROR_RESPONSE);
            size_t eoff = emsg.len;
            msgbuf_put_int32(&emsg, 0); /* placeholder */

            if (severity) { msgbuf_put_byte(&emsg, 'S'); msgbuf_put_string(&emsg, severity); }
            if (sqlstate) { msgbuf_put_byte(&emsg, 'C'); msgbuf_put_string(&emsg, sqlstate); }
            if (primary)  { msgbuf_put_byte(&emsg, 'M'); msgbuf_put_string(&emsg, primary); }
            if (detail)   { msgbuf_put_byte(&emsg, 'D'); msgbuf_put_string(&emsg, detail); }
            msgbuf_put_byte(&emsg, '\0');

            int32_t elen = (int32_t)(emsg.len - eoff);
            int32_t nelen = htonl(elen);
            memcpy(emsg.data + eoff, &nelen, 4);
            session_queue_send(session, emsg.data, emsg.len);
            msgbuf_free(&emsg);
            break;
        }
        default:
            break;
        }
        PQclear(res);
    }

    /* ReadyForQuery */
    char txn_status = TXN_IDLE;
    PGTransactionStatusType be_txn = PQtransactionStatus(session->backend_conn);
    if (be_txn == PQTRANS_INTRANS)
        txn_status = TXN_IN_TRANSACTION;
    else if (be_txn == PQTRANS_INERROR)
        txn_status = TXN_FAILED;

    MsgBuf msg;
    msgbuf_init(&msg, 16);
    msg_write_ready_for_query(&msg, txn_status);
    session_queue_send(session, msg.data, msg.len);
    msgbuf_free(&msg);

    /* Release pool connection if not in transaction */
    if (be_txn == PQTRANS_IDLE && session->backend_shard) {
        event_loop_del(session->loop, &session->backend_ev);
        conn_pool_release(&session->backend_shard->pool, session->backend_conn);
        session->backend_conn = NULL;
        session->backend_shard = NULL;
    }

    session->backend_busy = false;
    session->state = SESSION_STATE_READY;

    goto flush_and_return;

copy_out_read:
    /* Read COPY OUT data from backend and forward to client */
    {
        char *buf = NULL;
        int nbytes;
        while ((nbytes = PQgetCopyData(session->backend_conn, &buf, 1)) > 0) {
            /* Build CopyData message: type 'd' + len + data */
            MsgBuf cmsg;
            msgbuf_init(&cmsg, nbytes + 8);
            msgbuf_put_byte(&cmsg, MSG_COPY_DATA);
            msgbuf_put_int32(&cmsg, nbytes + 4);
            msgbuf_put_bytes(&cmsg, buf, nbytes);
            session_queue_send(session, cmsg.data, cmsg.len);
            msgbuf_free(&cmsg);
            PQfreemem(buf);
            buf = NULL;
        }
        if (buf) PQfreemem(buf);

        if (nbytes == -1) {
            /* COPY OUT done - send CopyDone to client */
            MsgBuf cmsg;
            msgbuf_init(&cmsg, 8);
            msgbuf_put_byte(&cmsg, MSG_COPY_DONE);
            msgbuf_put_int32(&cmsg, 4);
            session_queue_send(session, cmsg.data, cmsg.len);
            msgbuf_free(&cmsg);

            /* Get the final CommandComplete result */
            PGresult *final_res = PQgetResult(session->backend_conn);
            if (final_res) {
                if (PQresultStatus(final_res) == PGRES_COMMAND_OK) {
                    MsgBuf msg;
                    msgbuf_init(&msg, 64);
                    msg_write_command_complete(&msg, PQcmdStatus(final_res));
                    session_queue_send(session, msg.data, msg.len);
                    msgbuf_free(&msg);
                }
                PQclear(final_res);
            }

            char txn_status = TXN_IDLE;
            PGTransactionStatusType be_txn = PQtransactionStatus(session->backend_conn);
            if (be_txn == PQTRANS_INTRANS) txn_status = TXN_IN_TRANSACTION;
            else if (be_txn == PQTRANS_INERROR) txn_status = TXN_FAILED;

            MsgBuf msg;
            msgbuf_init(&msg, 16);
            msg_write_ready_for_query(&msg, txn_status);
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);

            if (be_txn == PQTRANS_IDLE && session->backend_shard) {
                event_loop_del(session->loop, &session->backend_ev);
                conn_pool_release(&session->backend_shard->pool, session->backend_conn);
                session->backend_conn = NULL;
                session->backend_shard = NULL;
            }

            session->state = SESSION_STATE_READY;
            log_info("COPY OUT completed");
        }
        /* nbytes == 0 means async not ready yet, wait for next readable event */
    }

flush_and_return:
    if (session_send(session) < 0) {
        session_destroy(session);
    }
}

/*
 * Scatter-gather: one of the shard backends is readable.
 */
void session_on_scatter_readable(EventHandler *handler, uint32_t events)
{
    ScatterConn *sc = (ScatterConn *)handler->data;
    ClientSession *session = sc->session;

    if (events & (EPOLLERR | EPOLLHUP)) {
        log_error("scatter backend error for shard '%s'", sc->shard->name);
        sc->done = true;
        session->scatter_done++;
        goto check_complete;
    }

    if (!sc->conn)
        return;

    if (PQconsumeInput(sc->conn) == 0) {
        log_error("scatter PQconsumeInput failed for shard '%s': %s",
                  sc->shard->name, PQerrorMessage(sc->conn));
        sc->done = true;
        session->scatter_done++;
        goto check_complete;
    }

    if (PQisBusy(sc->conn))
        return;

    PGresult *res;
    while ((res = PQgetResult(sc->conn)) != NULL) {
        ExecStatusType status = PQresultStatus(res);

        switch (status) {
        case PGRES_TUPLES_OK:
        case PGRES_SINGLE_TUPLE:
            session_forward_result(session, res, !session->scatter_header_sent);
            session->scatter_header_sent = true;
            break;

        case PGRES_COMMAND_OK:
            /* For DML, we'll send one combined CommandComplete at the end */
            break;

        case PGRES_FATAL_ERROR:
            log_error("scatter error from shard '%s': %s",
                      sc->shard->name, PQresultErrorMessage(res));
            break;

        default:
            break;
        }
        PQclear(res);
    }

    sc->done = true;
    session->scatter_done++;

check_complete:
    if (session->scatter_done >= session->scatter_count) {
        /* All shards done */
        if (!session->scatter_header_sent) {
            /* No rows returned from any shard, send empty result */
            MsgBuf msg;
            msgbuf_init(&msg, 64);
            msg_write_command_complete(&msg, "SELECT 0");
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
        } else {
            MsgBuf msg;
            msgbuf_init(&msg, 64);
            msg_write_command_complete(&msg, "SELECT");
            session_queue_send(session, msg.data, msg.len);
            msgbuf_free(&msg);
        }

        MsgBuf msg;
        msgbuf_init(&msg, 16);
        msg_write_ready_for_query(&msg, TXN_IDLE);
        session_queue_send(session, msg.data, msg.len);
        msgbuf_free(&msg);

        /* Release all scatter connections */
        session_release_scatter(session);
        session->state = SESSION_STATE_READY;

        if (session_send(session) < 0) {
            session_destroy(session);
            return;
        }
    }
}
