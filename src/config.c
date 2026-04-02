#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char* trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static char* my_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

ShagresCfg* config_default(void)
{
    ShagresCfg *cfg = calloc(1, sizeof(ShagresCfg));
    cfg->listen_addr = strdup("0.0.0.0");
    cfg->listen_port = 15432;
    cfg->max_clients = 1024;
    cfg->log_level = LOG_INFO;
    cfg->auth_method = 0;
    cfg->default_pool_size = 20;
    cfg->max_pool_size = 100;
    cfg->idle_timeout = 300;
    cfg->backend_conninfo = strdup("host=127.0.0.1 port=5432 dbname=postgres");
    return cfg;
}

/* Find a shard by name in the temp arrays during config load */
static int find_shard_index(Shard *shards, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(shards[i].name, name) == 0)
            return i;
    }
    return -1;
}

ShagresCfg* config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        log_error("cannot open config file: %s", path);
        return NULL;
    }

    ShagresCfg *cfg = config_default();

    /* Temp storage for shards and table rules during parsing */
    Shard tmp_shards[MAX_SHARDS];
    int tmp_shard_count = 0;

    typedef struct {
        char table_name[128];
        char shard_column[128];
        char method[16];
        char shard_list[1024];
        int  broadcast;
    } TmpTableRule;

    TmpTableRule tmp_rules[MAX_TABLES];
    int tmp_rule_count = 0;

    char line[1024];
    char current_section[128] = "";
    char current_section_arg[128] = "";  /* e.g., "shard_0" from [shard:shard_0] */

    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        /* Section header */
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                char *section = p + 1;
                char *colon = strchr(section, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(current_section, section, sizeof(current_section) - 1);
                    strncpy(current_section_arg, colon + 1, sizeof(current_section_arg) - 1);
                } else {
                    strncpy(current_section, section, sizeof(current_section) - 1);
                    current_section_arg[0] = '\0';
                }
            }
            continue;
        }

        /* Key = Value */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (strcmp(current_section, "global") == 0) {
            if (strcmp(key, "listen_addr") == 0) {
                free(cfg->listen_addr);
                cfg->listen_addr = strdup(val);
            } else if (strcmp(key, "listen_port") == 0) {
                cfg->listen_port = atoi(val);
            } else if (strcmp(key, "max_clients") == 0) {
                cfg->max_clients = atoi(val);
            } else if (strcmp(key, "log_level") == 0) {
                if (strcmp(val, "debug") == 0) cfg->log_level = LOG_DEBUG;
                else if (strcmp(val, "info") == 0) cfg->log_level = LOG_INFO;
                else if (strcmp(val, "warn") == 0) cfg->log_level = LOG_WARN;
                else if (strcmp(val, "error") == 0) cfg->log_level = LOG_ERROR;
            }
        } else if (strcmp(current_section, "auth") == 0) {
            if (strcmp(key, "method") == 0) {
                if (strcmp(val, "trust") == 0) cfg->auth_method = 0;
            }
        } else if (strcmp(current_section, "pool") == 0) {
            if (strcmp(key, "default_pool_size") == 0)
                cfg->default_pool_size = atoi(val);
            else if (strcmp(key, "max_pool_size") == 0)
                cfg->max_pool_size = atoi(val);
            else if (strcmp(key, "idle_timeout") == 0)
                cfg->idle_timeout = atoi(val);
        } else if (strcmp(current_section, "backend") == 0) {
            if (strcmp(key, "conninfo") == 0) {
                free(cfg->backend_conninfo);
                cfg->backend_conninfo = strdup(val);
            }
        } else if (strcmp(current_section, "shard") == 0 && current_section_arg[0]) {
            if (tmp_shard_count >= MAX_SHARDS) continue;

            /* Find or create shard entry */
            int idx = find_shard_index(tmp_shards, tmp_shard_count, current_section_arg);
            if (idx < 0) {
                idx = tmp_shard_count++;
                memset(&tmp_shards[idx], 0, sizeof(Shard));
                tmp_shards[idx].name = strdup(current_section_arg);
            }

            if (strcmp(key, "conninfo") == 0) {
                free(tmp_shards[idx].conninfo);
                tmp_shards[idx].conninfo = strdup(val);
            } else if (strcmp(key, "key_range_start") == 0) {
                tmp_shards[idx].key_range_start = strtoull(val, NULL, 10);
            } else if (strcmp(key, "key_range_end") == 0) {
                tmp_shards[idx].key_range_end = strtoull(val, NULL, 10);
            }
        } else if (strcmp(current_section, "table") == 0 && current_section_arg[0]) {
            if (tmp_rule_count >= MAX_TABLES) continue;

            /* Find or create table rule entry */
            int idx = -1;
            for (int i = 0; i < tmp_rule_count; i++) {
                if (strcmp(tmp_rules[i].table_name, current_section_arg) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                idx = tmp_rule_count++;
                memset(&tmp_rules[idx], 0, sizeof(TmpTableRule));
                strncpy(tmp_rules[idx].table_name, current_section_arg,
                        sizeof(tmp_rules[idx].table_name) - 1);
            }

            if (strcmp(key, "shard_column") == 0)
                strncpy(tmp_rules[idx].shard_column, val, sizeof(tmp_rules[idx].shard_column) - 1);
            else if (strcmp(key, "method") == 0)
                strncpy(tmp_rules[idx].method, val, sizeof(tmp_rules[idx].method) - 1);
            else if (strcmp(key, "shards") == 0)
                strncpy(tmp_rules[idx].shard_list, val, sizeof(tmp_rules[idx].shard_list) - 1);
            else if (strcmp(key, "broadcast") == 0)
                tmp_rules[idx].broadcast = (strcmp(val, "true") == 0);
        }
    }

    fclose(f);

    /* Build ShardMap from parsed data */
    ShardMap *map = &cfg->shard_map;
    map->pool_size = cfg->default_pool_size;

    if (tmp_shard_count > 0) {
        map->shard_count = tmp_shard_count;
        map->shards = calloc(tmp_shard_count, sizeof(Shard));
        for (int i = 0; i < tmp_shard_count; i++) {
            map->shards[i].name = tmp_shards[i].name;
            map->shards[i].conninfo = tmp_shards[i].conninfo;
            map->shards[i].key_range_start = tmp_shards[i].key_range_start;
            map->shards[i].key_range_end = tmp_shards[i].key_range_end;
        }
    }

    if (tmp_rule_count > 0) {
        map->rule_count = tmp_rule_count;
        map->table_rules = calloc(tmp_rule_count, sizeof(TableRule));
        for (int i = 0; i < tmp_rule_count; i++) {
            TableRule *r = &map->table_rules[i];
            r->table_name = strdup(tmp_rules[i].table_name);
            r->shard_column = my_strdup(tmp_rules[i].shard_column[0] ? tmp_rules[i].shard_column : NULL);
            r->broadcast = tmp_rules[i].broadcast;

            if (strcmp(tmp_rules[i].method, "hash") == 0)
                r->method = SHARD_HASH;
            else
                r->method = SHARD_RANGE;

            /* Parse shard list "shard_0, shard_1" */
            if (tmp_rules[i].shard_list[0] && map->shard_count > 0) {
                char *list = strdup(tmp_rules[i].shard_list);
                char *tok = strtok(list, ",");
                int max_shards = map->shard_count;
                r->shards = calloc(max_shards, sizeof(Shard *));
                r->shard_count = 0;

                while (tok && r->shard_count < max_shards) {
                    tok = trim(tok);
                    int idx = find_shard_index(map->shards, map->shard_count, tok);
                    if (idx >= 0) {
                        r->shards[r->shard_count++] = &map->shards[idx];
                    } else {
                        log_warn("config: table '%s' references unknown shard '%s'",
                                 r->table_name, tok);
                    }
                    tok = strtok(NULL, ",");
                }
                free(list);
            } else if (r->broadcast && map->shard_count > 0) {
                /* Broadcast: include all shards */
                r->shards = calloc(map->shard_count, sizeof(Shard *));
                r->shard_count = map->shard_count;
                for (int j = 0; j < map->shard_count; j++)
                    r->shards[j] = &map->shards[j];
            }
        }
    }

    log_info("config loaded: %d shards, %d table rules from %s",
             map->shard_count, map->rule_count, path);
    return cfg;
}

void config_free(ShagresCfg *cfg)
{
    if (!cfg) return;
    free(cfg->listen_addr);
    free(cfg->backend_conninfo);
    shard_map_destroy(&cfg->shard_map);
    free(cfg);
}
