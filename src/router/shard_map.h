#ifndef SHAREGRES_SHARD_MAP_H
#define SHAREGRES_SHARD_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "../pool/conn_pool.h"

/* Sharding method */
typedef enum {
    SHARD_RANGE,
    SHARD_HASH,
} ShardMethod;

/* A single shard definition */
typedef struct Shard {
    char           *name;            /* e.g. "shard_0" */
    char           *conninfo;        /* libpq connection string */
    uint64_t        key_range_start; /* inclusive */
    uint64_t        key_range_end;   /* exclusive */
    ConnPool        pool;            /* connection pool for this shard */
} Shard;

/* Table sharding rule */
typedef struct TableRule {
    char           *table_name;
    char           *shard_column;    /* column to shard on */
    ShardMethod     method;
    bool            broadcast;       /* if true, send to all shards */
    int             shard_count;
    Shard         **shards;          /* pointers into global shard array */
} TableRule;

/* Global shard map */
typedef struct ShardMap {
    Shard          *shards;
    int             shard_count;
    TableRule      *table_rules;
    int             rule_count;
    int             pool_size;       /* default pool size per shard */
} ShardMap;

/* Initialize shard map (called after config load) */
int         shard_map_init(ShardMap *map);
void        shard_map_destroy(ShardMap *map);

/* Find the table rule for a given table name */
TableRule*  shard_map_find_rule(ShardMap *map, const char *table_name);

/* Find the target shard for a given integer key value */
Shard*      shard_map_find_by_int(TableRule *rule, int64_t key);

/* Find the target shard for a given string key value (hashed) */
Shard*      shard_map_find_by_str(TableRule *rule, const char *key);

/* Get all shards for a table rule (for scatter queries) */
int         shard_map_get_all(TableRule *rule, Shard ***out_shards);

#endif /* SHAREGRES_SHARD_MAP_H */
