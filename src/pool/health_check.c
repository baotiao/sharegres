#include "health_check.h"
#include "../log.h"

void health_check_all(ShardMap *map)
{
    if (!map || map->shard_count == 0)
        return;

    for (int i = 0; i < map->shard_count; i++) {
        Shard *s = &map->shards[i];
        ConnPool *pool = &s->pool;

        conn_pool_health_check(pool);

        log_debug("health: shard '%s' pool: idle=%d active=%d max=%d",
                  s->name, pool->idle_count, pool->active_count, pool->max_size);
    }
}
