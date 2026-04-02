#include "conn_pool.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>

void conn_pool_init(ConnPool *pool, const char *conninfo, int max_size)
{
    memset(pool, 0, sizeof(ConnPool));
    pool->conninfo = strdup(conninfo);
    pool->max_size = max_size;
    pool->idle_conns = calloc(max_size, sizeof(PGconn *));
    pool->idle_count = 0;
    pool->active_count = 0;
}

void conn_pool_destroy(ConnPool *pool)
{
    for (int i = 0; i < pool->idle_count; i++) {
        if (pool->idle_conns[i]) {
            PQfinish(pool->idle_conns[i]);
        }
    }
    free(pool->idle_conns);
    free(pool->conninfo);
    memset(pool, 0, sizeof(ConnPool));
}

PGconn* conn_pool_acquire(ConnPool *pool)
{
    /* Try to reuse an idle connection */
    while (pool->idle_count > 0) {
        PGconn *conn = pool->idle_conns[--pool->idle_count];
        pool->idle_conns[pool->idle_count] = NULL;

        /* Verify connection is still good */
        if (PQstatus(conn) == CONNECTION_OK) {
            pool->active_count++;
            return conn;
        }

        /* Connection is dead, close it */
        log_warn("pool: closing dead connection to %s", pool->conninfo);
        PQfinish(conn);
    }

    /* No idle connections, create a new one if under limit */
    if (pool->active_count >= pool->max_size) {
        log_error("pool: max connections reached (%d) for %s",
                  pool->max_size, pool->conninfo);
        return NULL;
    }

    log_debug("pool: creating new connection to %s", pool->conninfo);
    PGconn *conn = PQconnectdb(pool->conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        log_error("pool: connect failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }

    pool->active_count++;
    return conn;
}

void conn_pool_release(ConnPool *pool, PGconn *conn)
{
    if (!conn)
        return;

    pool->active_count--;

    /* Check if connection is still usable */
    if (PQstatus(conn) != CONNECTION_OK) {
        log_warn("pool: releasing dead connection");
        PQfinish(conn);
        return;
    }

    /* If the connection is in a transaction, close it rather than
     * issuing a blocking DISCARD ALL that could hang the event loop. */
    PGTransactionStatusType txn = PQtransactionStatus(conn);
    if (txn != PQTRANS_IDLE) {
        PQfinish(conn);
        return;
    }

    /* Return to pool if there's room */
    if (pool->idle_count < pool->max_size) {
        pool->idle_conns[pool->idle_count++] = conn;
    } else {
        PQfinish(conn);
    }
}

void conn_pool_health_check(ConnPool *pool)
{
    int removed = 0;
    for (int i = pool->idle_count - 1; i >= 0; i--) {
        if (PQstatus(pool->idle_conns[i]) != CONNECTION_OK) {
            PQfinish(pool->idle_conns[i]);
            /* Shift remaining entries */
            for (int j = i; j < pool->idle_count - 1; j++)
                pool->idle_conns[j] = pool->idle_conns[j + 1];
            pool->idle_count--;
            removed++;
        }
    }
    if (removed > 0) {
        log_info("pool: health check removed %d dead connections from %s",
                 removed, pool->conninfo);
    }
}
