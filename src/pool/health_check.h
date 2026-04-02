#ifndef SHAREGRES_HEALTH_CHECK_H
#define SHAREGRES_HEALTH_CHECK_H

#include "../router/shard_map.h"

/*
 * Run health checks on all shard connection pools.
 * - Closes dead idle connections
 * - Logs pool statistics
 */
void health_check_all(ShardMap *map);

#endif /* SHAREGRES_HEALTH_CHECK_H */
