#include "shard_map.h"
#include "../log.h"

#include <stdlib.h>
#include <string.h>

int shard_map_init(ShardMap *map)
{
    /* Initialize connection pools for each shard */
    for (int i = 0; i < map->shard_count; i++) {
        conn_pool_init(&map->shards[i].pool,
                       map->shards[i].conninfo,
                       map->pool_size);
        log_info("shard '%s': range [%lu, %lu) -> %s",
                 map->shards[i].name,
                 map->shards[i].key_range_start,
                 map->shards[i].key_range_end,
                 map->shards[i].conninfo);
    }

    for (int i = 0; i < map->rule_count; i++) {
        TableRule *r = &map->table_rules[i];
        if (r->broadcast) {
            log_info("table '%s': broadcast to all shards", r->table_name);
        } else {
            log_info("table '%s': shard on column '%s' (%s), %d shards",
                     r->table_name, r->shard_column,
                     r->method == SHARD_RANGE ? "range" : "hash",
                     r->shard_count);
        }
    }

    return 0;
}

void shard_map_destroy(ShardMap *map)
{
    for (int i = 0; i < map->shard_count; i++) {
        conn_pool_destroy(&map->shards[i].pool);
        free(map->shards[i].name);
        free(map->shards[i].conninfo);
    }
    free(map->shards);

    for (int i = 0; i < map->rule_count; i++) {
        free(map->table_rules[i].table_name);
        free(map->table_rules[i].shard_column);
        free(map->table_rules[i].shards);
    }
    free(map->table_rules);
}

TableRule* shard_map_find_rule(ShardMap *map, const char *table_name)
{
    if (!table_name)
        return NULL;

    for (int i = 0; i < map->rule_count; i++) {
        if (strcmp(map->table_rules[i].table_name, table_name) == 0)
            return &map->table_rules[i];
    }
    return NULL;
}

Shard* shard_map_find_by_int(TableRule *rule, int64_t key)
{
    if (!rule || rule->shard_count == 0)
        return NULL;

    if (rule->method == SHARD_RANGE) {
        uint64_t ukey = (uint64_t)key;
        for (int i = 0; i < rule->shard_count; i++) {
            Shard *s = rule->shards[i];
            if (ukey >= s->key_range_start && ukey < s->key_range_end)
                return s;
        }
        /* Key not in any range - fall back to first shard */
        log_warn("key %ld not in any shard range, using first shard", key);
        return rule->shards[0];
    }

    if (rule->method == SHARD_HASH) {
        int idx = (int)(((uint64_t)key) % rule->shard_count);
        return rule->shards[idx];
    }

    return NULL;
}

Shard* shard_map_find_by_str(TableRule *rule, const char *key)
{
    if (!rule || !key || rule->shard_count == 0)
        return NULL;

    /* Simple hash for strings */
    uint64_t hash = 5381;
    for (const char *p = key; *p; p++)
        hash = ((hash << 5) + hash) + (uint64_t)*p;

    if (rule->method == SHARD_RANGE) {
        /* Use hash value as if it were a range key */
        for (int i = 0; i < rule->shard_count; i++) {
            Shard *s = rule->shards[i];
            if (hash >= s->key_range_start && hash < s->key_range_end)
                return s;
        }
        return rule->shards[0];
    }

    int idx = (int)(hash % rule->shard_count);
    return rule->shards[idx];
}

int shard_map_get_all(TableRule *rule, Shard ***out_shards)
{
    *out_shards = rule->shards;
    return rule->shard_count;
}
