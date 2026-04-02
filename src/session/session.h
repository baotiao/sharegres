#ifndef SHAREGRES_SESSION_H
#define SHAREGRES_SESSION_H

#include <stdbool.h>
#include <libpq-fe.h>
#include "../event/event_loop.h"
#include "../protocol/message.h"
#include "../router/shard_map.h"

/* Forward declare */
typedef struct ShardMap ShardMap;

/* Client session states */
typedef enum {
    SESSION_STATE_INIT,          /* waiting for StartupMessage */
    SESSION_STATE_AUTH,          /* authentication in progress */
    SESSION_STATE_READY,         /* ready for queries */
    SESSION_STATE_QUERY,         /* executing a query (single shard) */
    SESSION_STATE_SCATTER,       /* executing scatter-gather */
    SESSION_STATE_COPY_IN,       /* COPY IN: forwarding client data to backend */
    SESSION_STATE_COPY_OUT,      /* COPY OUT: forwarding backend data to client */
    SESSION_STATE_CLOSING,       /* connection being closed */
} SessionState;

/* Transaction states */
typedef enum {
    TXN_STATE_IDLE,
    TXN_STATE_IN_TRANSACTION,
    TXN_STATE_FAILED,
} TxnState;

/* Receive buffer for partial message reassembly */
typedef struct RecvBuf {
    uint8_t    *data;
    size_t      len;
    size_t      cap;
} RecvBuf;

/* Per-shard backend state for scatter-gather */
#define MAX_SCATTER_SHARDS 64

typedef struct ClientSession ClientSession;

typedef struct ScatterConn {
    Shard          *shard;
    PGconn         *conn;
    EventHandler    ev;
    bool            done;        /* result fully consumed */
    bool            from_pool;   /* was acquired from pool (needs release) */
    ClientSession  *session;     /* back-pointer to owning session */
} ScatterConn;

/*
 * Client session: one per connected client.
 */
typedef struct ClientSession {
    int             client_fd;
    SessionState    state;
    TxnState        txn_state;

    /* Event handler for epoll */
    EventHandler    ev_handler;

    /* Receive buffer for incoming client data */
    RecvBuf         recv_buf;

    /* Send buffer for outgoing data to client */
    MsgBuf          send_buf;
    size_t          send_pos;

    /* Single-shard backend connection */
    PGconn         *backend_conn;
    EventHandler    backend_ev;
    bool            backend_busy;
    Shard          *backend_shard;    /* which shard this conn belongs to */

    /* Scatter-gather state */
    ScatterConn     scatter[MAX_SCATTER_SHARDS];
    int             scatter_count;
    int             scatter_done;    /* how many shards finished */
    bool            scatter_header_sent; /* RowDescription already sent */

    /* Session parameters from startup message */
    char           *user;
    char           *database;

    /* Backend key data (for cancel) */
    int32_t         pid;
    int32_t         secret_key;

    /* Pointer back to event loop */
    EventLoop      *loop;

    /* Routing context */
    ShardMap       *shard_map;       /* global shard map (not owned) */
    const char     *backend_conninfo; /* fallback backend */
} ClientSession;

/* Session lifecycle */
ClientSession* session_create(int client_fd, EventLoop *loop,
                              const char *backend_conninfo, ShardMap *shard_map);
void           session_destroy(ClientSession *session);

/* Protocol processing */
void           session_on_client_readable(EventHandler *handler, uint32_t events);
void           session_on_client_writable(EventHandler *handler, uint32_t events);
void           session_on_backend_readable(EventHandler *handler, uint32_t events);

/* Scatter-gather backend callback */
void           session_on_scatter_readable(EventHandler *handler, uint32_t events);

/* Send helpers */
int            session_send(ClientSession *session);
void           session_queue_send(ClientSession *session, const void *data, size_t len);

/* Receive buffer helpers */
void           recvbuf_init(RecvBuf *rb, size_t cap);
void           recvbuf_free(RecvBuf *rb);

#endif /* SHAREGRES_SESSION_H */
