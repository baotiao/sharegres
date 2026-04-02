#include "json_util.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

const char* json_skip_whitespace(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Internal alias for brevity */
static inline const char* skip_whitespace(const char *p)
{
    return json_skip_whitespace(p);
}

/* Skip a JSON string (starting at the opening quote).
 * Returns pointer after the closing quote. */
static const char* skip_string(const char *p)
{
    if (*p != '"') return p;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') p++; /* skip escaped char */
        p++;
    }
    if (*p == '"') p++;
    return p;
}

const char* json_skip_value(const char *p)
{
    p = skip_whitespace(p);

    switch (*p) {
    case '"':
        return skip_string(p);

    case '{': {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p = skip_string(p); continue; }
            p++;
        }
        return p;
    }

    case '[': {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p = skip_string(p); continue; }
            p++;
        }
        return p;
    }

    default:
        /* number, true, false, null */
        while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p))
            p++;
        return p;
    }
}

const char* json_find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    json = skip_whitespace(json);
    if (*json != '{') return NULL;
    json++;

    size_t keylen = strlen(key);

    while (*json && *json != '}') {
        json = skip_whitespace(json);
        if (*json != '"') break;

        /* Check if this key matches */
        if (strncmp(json + 1, key, keylen) == 0 && json[1 + keylen] == '"') {
            /* Found key, skip to value */
            json += 2 + keylen; /* past closing quote */
            json = skip_whitespace(json);
            if (*json == ':') json++;
            return skip_whitespace(json);
        }

        /* Skip this key */
        json = skip_string(json);
        json = skip_whitespace(json);
        if (*json == ':') json++;
        json = skip_whitespace(json);

        /* Skip value */
        json = json_skip_value(json);
        json = skip_whitespace(json);
        if (*json == ',') json++;
    }

    return NULL;
}

char* json_get_string(const char *json)
{
    if (!json) return NULL;
    json = skip_whitespace(json);
    if (*json != '"') return NULL;

    json++; /* skip opening quote */
    const char *start = json;
    while (*json && *json != '"') {
        if (*json == '\\') json++;
        json++;
    }

    size_t len = json - start;
    char *result = malloc(len + 1);
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

int64_t json_get_int(const char *json)
{
    if (!json) return 0;
    json = skip_whitespace(json);
    return strtoll(json, NULL, 10);
}

bool json_starts_with_key(const char *json, const char *key)
{
    if (!json || !key) return false;
    json = skip_whitespace(json);
    if (*json != '{') return false;
    json = skip_whitespace(json + 1);
    if (*json != '"') return false;

    size_t keylen = strlen(key);
    return (strncmp(json + 1, key, keylen) == 0 && json[1 + keylen] == '"');
}

const char* json_find_key_recursive(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    json = skip_whitespace(json);

    if (*json == '{') {
        /* First check this level */
        const char *val = json_find_key(json, key);
        if (val) return val;

        /* Then recurse into values */
        const char *p = json + 1;
        while (*p && *p != '}') {
            p = skip_whitespace(p);
            if (*p == '"') {
                p = skip_string(p); /* skip key */
                p = skip_whitespace(p);
                if (*p == ':') p++;
                p = skip_whitespace(p);

                /* Recurse into value */
                const char *found = json_find_key_recursive(p, key);
                if (found) return found;

                p = json_skip_value(p);
                p = skip_whitespace(p);
                if (*p == ',') p++;
            } else {
                p++;
            }
        }
    } else if (*json == '[') {
        const char *p = json + 1;
        while (*p && *p != ']') {
            p = skip_whitespace(p);
            if (*p == ']') break;

            const char *found = json_find_key_recursive(p, key);
            if (found) return found;

            p = json_skip_value(p);
            p = skip_whitespace(p);
            if (*p == ',') p++;
        }
    }

    return NULL;
}

const char** json_find_all_values(const char *json, const char *key, int *count)
{
    *count = 0;
    if (!json || !key) return NULL;

    int cap = 8;
    const char **results = malloc(cap * sizeof(const char *));

    /* Simple recursive search using a stack-based approach */
    /* We'll do a linear scan through the JSON looking for the key */
    size_t keylen = strlen(key);
    const char *p = json;

    while (*p) {
        /* Look for a quoted key that matches */
        if (*p == '"') {
            if (strncmp(p + 1, key, keylen) == 0 && p[1 + keylen] == '"') {
                /* Potential match - check if it's followed by ':' */
                const char *after = skip_whitespace(p + 2 + keylen);
                if (*after == ':') {
                    const char *val = skip_whitespace(after + 1);
                    if (*count >= cap) {
                        cap *= 2;
                        results = realloc(results, cap * sizeof(const char *));
                    }
                    results[(*count)++] = val;
                    p = json_skip_value(val);
                    continue;
                }
            }
            p = skip_string(p);
        } else {
            p++;
        }
    }

    return results;
}
