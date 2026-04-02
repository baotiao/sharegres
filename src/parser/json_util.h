#ifndef SHAREGRES_JSON_UTIL_H
#define SHAREGRES_JSON_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Minimal JSON navigator for libpg_query parse tree output.
 * Not a full parser - just enough to find keys and extract values.
 */

/* Find a key in a JSON object, return pointer to the value start.
 * Input `json` should point to the '{' of the object.
 * Returns NULL if key not found. */
const char* json_find_key(const char *json, const char *key);

/* Extract a string value (without quotes). Caller must free. */
char* json_get_string(const char *json);

/* Extract an integer value. */
int64_t json_get_int(const char *json);

/* Check if a JSON value starts with a specific object key.
 * e.g., json_starts_with_key(p, "SelectStmt") checks if p is {"SelectStmt":...} */
bool json_starts_with_key(const char *json, const char *key);

/* Skip over a JSON value (object, array, string, number, etc.)
 * Returns pointer to the character after the value. */
const char* json_skip_value(const char *json);

/* Find a key recursively in nested JSON. Returns first match.
 * Useful for finding "relname" anywhere in the tree. */
const char* json_find_key_recursive(const char *json, const char *key);

/* Extract all values for a given key recursively.
 * Returns array of pointers (caller frees the array, not the contents).
 * Sets *count to number of matches. */
const char** json_find_all_values(const char *json, const char *key, int *count);

/* Skip whitespace (exposed for use by other modules) */
const char* json_skip_whitespace(const char *p);

#endif /* SHAREGRES_JSON_UTIL_H */
