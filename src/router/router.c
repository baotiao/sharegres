#include "router.h"
#include "../log.h"

#include <string.h>

void router_decide(ShardMap *map, ParseResult *parsed, RouteDecision *decision)
{
    memset(decision, 0, sizeof(RouteDecision));

    /* Local statements don't hit any backend */
    if (parsed->stmt_type == STMT_SET ||
        parsed->stmt_type == STMT_SHOW ||
        parsed->stmt_type == STMT_DISCARD) {
        decision->type = ROUTE_LOCAL;
        return;
    }

    /* Transaction statements: route to default for now */
    if (parsed->stmt_type == STMT_TRANSACTION) {
        decision->type = ROUTE_DEFAULT;
        return;
    }

    /* Unknown statement or no table: default backend */
    if (parsed->stmt_type == STMT_UNKNOWN || !parsed->table_name) {
        decision->type = ROUTE_DEFAULT;
        return;
    }

    /* Find table rule */
    TableRule *rule = shard_map_find_rule(map, parsed->table_name);
    if (!rule) {
        /* No sharding rule for this table - use default/scatter */
        log_debug("router: no rule for table '%s', using default", parsed->table_name);
        decision->type = ROUTE_DEFAULT;
        return;
    }

    decision->rule = rule;

    /* Broadcast tables always go to all shards */
    if (rule->broadcast) {
        decision->type = ROUTE_SCATTER;
        decision->target_count = shard_map_get_all(rule, &decision->target_shards);
        log_debug("router: broadcast table '%s' -> %d shards",
                  parsed->table_name, decision->target_count);
        return;
    }

    /* DDL always scatter */
    if (parsed->stmt_type == STMT_DDL) {
        decision->type = ROUTE_SCATTER;
        decision->target_count = shard_map_get_all(rule, &decision->target_shards);
        log_debug("router: DDL for '%s' -> scatter to %d shards",
                  parsed->table_name, decision->target_count);
        return;
    }

    /* For INSERT, try to extract shard key if we haven't yet */
    if (parsed->stmt_type == STMT_INSERT && !parsed->shard_key_found) {
        parse_extract_insert_shard_key(parsed, rule->shard_column);
    }

    /* Check if shard key was found and matches the rule's column */
    if (parsed->shard_key_found && parsed->shard_key_column &&
        strcmp(parsed->shard_key_column, rule->shard_column) == 0) {

        Shard *target = NULL;
        if (parsed->shard_key_is_int) {
            target = shard_map_find_by_int(rule, parsed->shard_key_int);
        } else if (parsed->shard_key_str) {
            target = shard_map_find_by_str(rule, parsed->shard_key_str);
        }

        if (target) {
            decision->type = ROUTE_SINGLE;
            decision->_single_shard = target;
            decision->target_shards = &decision->_single_shard;
            decision->target_count = 1;
            log_debug("router: table '%s' key=%s -> shard '%s'",
                      parsed->table_name,
                      parsed->shard_key_is_int ? "int" : "str",
                      target->name);
            return;
        }
    }

    /* No shard key found or doesn't match column - scatter */
    decision->type = ROUTE_SCATTER;
    decision->target_count = shard_map_get_all(rule, &decision->target_shards);
    log_debug("router: table '%s' no shard key -> scatter to %d shards",
              parsed->table_name, decision->target_count);
}
