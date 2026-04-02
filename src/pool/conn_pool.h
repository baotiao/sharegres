#ifndef SHAREGRES_CONN_POOL_H
#define SHAREGRES_CONN_POOL_H

#include <libpq-fe.h>
#include <stdbool.h>

/*
 * Per-shard connection pool.
 * Manages a set of PGconn handles with lazy creation.
 */
typedef struct ConnPool {
    char           *conninfo;        /* libpq connection string */
    PGconn        **idle_conns;      /* array of idle connections */
    int             idle_count;
    int             max_size;
    int             active_count;    /* connections currently checked out */
} ConnPool;

/* Initialize pool (does not create connections yet) */
void    conn_pool_init(ConnPool *pool, const char *conninfo, int max_size);

/* Destroy pool: close all connections */
void    conn_pool_destroy(ConnPool *pool);

/* Get a connection (may block to create new one).
 * Returns NULL on failure. */
PGconn* conn_pool_acquire(ConnPool *pool);

/* Return a connection to the pool.
 * If conn is in bad state, it's closed instead. */
void    conn_pool_release(ConnPool *pool, PGconn *conn);

/* Close idle connections that have been idle too long (placeholder). */
void    conn_pool_health_check(ConnPool *pool);

#endif /* SHAREGRES_CONN_POOL_H */
