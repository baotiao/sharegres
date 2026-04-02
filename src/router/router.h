#ifndef SHAREGRES_ROUTER_H
#define SHAREGRES_ROUTER_H

#include "shard_map.h"
#include "../parser/query_parser.h"

/* Route decision type */
typedef enum {
    ROUTE_SINGLE,       /* route to exactly one shard */
    ROUTE_SCATTER,      /* broadcast to all shards */
    ROUTE_LOCAL,        /* handle locally (SET/SHOW) */
    ROUTE_DEFAULT,      /* use default/single backend (unknown table) */
} RouteType;

/* Route decision */
typedef struct {
    RouteType       type;
    Shard         **target_shards;   /* array of target shards (owned by shard_map) */
    int             target_count;
    TableRule      *rule;            /* matched table rule (or NULL) */
    Shard          *_single_shard;   /* inline storage for ROUTE_SINGLE (avoids static) */
} RouteDecision;

/*
 * Determine routing for a parsed query.
 * `parsed` should already have been processed by parse_query().
 * The RouteDecision's target_shards are borrowed pointers, not owned.
 */
void    router_decide(ShardMap *map, ParseResult *parsed, RouteDecision *decision);

#endif /* SHAREGRES_ROUTER_H */
