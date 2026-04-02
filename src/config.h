#ifndef SHAREGRES_CONFIG_H
#define SHAREGRES_CONFIG_H

#include "log.h"
#include "router/shard_map.h"

#define MAX_SHARDS  64
#define MAX_TABLES  64

typedef struct ShagresCfg {
    /* [global] */
    char       *listen_addr;
    int         listen_port;
    int         max_clients;
    LogLevel    log_level;

    /* [auth] */
    int         auth_method;    /* 0=trust */

    /* [pool] */
    int         default_pool_size;
    int         max_pool_size;
    int         idle_timeout;

    /* [backend] - fallback single backend for unsharded tables */
    char       *backend_conninfo;

    /* Shard map (populated from [shard:*] and [table:*] sections) */
    ShardMap    shard_map;
} ShagresCfg;

ShagresCfg* config_load(const char *path);
ShagresCfg* config_default(void);
void        config_free(ShagresCfg *cfg);

#endif /* SHAREGRES_CONFIG_H */
