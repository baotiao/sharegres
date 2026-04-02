#include "query_parser.h"
#include "json_util.h"
#include "../log.h"

#include <pg_query.h>
#include <string.h>
#include <stdlib.h>

/* Use json_skip_ws from json_util */
static inline const char* skip_ws(const char *p)
{
    return json_skip_whitespace(p);
}

/*
 * Detect statement type from the top-level key in the stmt object.
 */
static StmtType detect_stmt_type(const char *stmt_json)
{
    if (json_starts_with_key(stmt_json, "SelectStmt"))
        return STMT_SELECT;
    if (json_starts_with_key(stmt_json, "InsertStmt"))
        return STMT_INSERT;
    if (json_starts_with_key(stmt_json, "UpdateStmt"))
        return STMT_UPDATE;
    if (json_starts_with_key(stmt_json, "DeleteStmt"))
        return STMT_DELETE;
    if (json_starts_with_key(stmt_json, "TransactionStmt"))
        return STMT_TRANSACTION;
    if (json_starts_with_key(stmt_json, "VariableSetStmt"))
        return STMT_SET;
    if (json_starts_with_key(stmt_json, "VariableShowStmt"))
        return STMT_SHOW;
    if (json_starts_with_key(stmt_json, "DiscardStmt"))
        return STMT_DISCARD;
    if (json_starts_with_key(stmt_json, "CopyStmt"))
        return STMT_COPY;

    /* DDL types */
    if (json_starts_with_key(stmt_json, "CreateStmt") ||
        json_starts_with_key(stmt_json, "AlterTableStmt") ||
        json_starts_with_key(stmt_json, "DropStmt") ||
        json_starts_with_key(stmt_json, "IndexStmt") ||
        json_starts_with_key(stmt_json, "CreateSchemaStmt") ||
        json_starts_with_key(stmt_json, "ViewStmt") ||
        json_starts_with_key(stmt_json, "CreateFunctionStmt") ||
        json_starts_with_key(stmt_json, "TruncateStmt") ||
        json_starts_with_key(stmt_json, "RenameStmt") ||
        json_starts_with_key(stmt_json, "CreateSeqStmt") ||
        json_starts_with_key(stmt_json, "AlterSeqStmt"))
        return STMT_DDL;

    return STMT_UNKNOWN;
}

/*
 * Extract table name from a RangeVar node.
 * A RangeVar looks like: {"relname":"orders","inh":true,...}
 */
static void extract_table_from_rangevar(const char *rv_json, ParseResult *result)
{
    const char *relname = json_find_key(rv_json, "relname");
    if (relname) {
        free(result->table_name);
        result->table_name = json_get_string(relname);
    }
    const char *schemaname = json_find_key(rv_json, "schemaname");
    if (schemaname) {
        free(result->schema_name);
        result->schema_name = json_get_string(schemaname);
    }
}

/*
 * Extract shard key from A_Expr (equality condition).
 * Looking for: {"A_Expr":{"kind":"AEXPR_OP","name":[{"String":{"sval":"="}}],
 *   "lexpr":{"ColumnRef":{"fields":[{"String":{"sval":"user_id"}}],...}},
 *   "rexpr":{"A_Const":{"ival":{"ival":42},...}},...}}
 */
static void extract_shard_key_from_where(const char *where_json, ParseResult *result)
{
    if (!where_json)
        return;

    /* Look for A_Expr with = operator */
    const char *aexpr = json_find_key(where_json, "A_Expr");
    if (!aexpr) {
        /* Could be BoolExpr (AND/OR) - try to recurse into args */
        const char *boolexpr = json_find_key(where_json, "BoolExpr");
        if (boolexpr) {
            const char *args = json_find_key(boolexpr, "args");
            if (args && *args == '[') {
                /* Iterate over array elements */
                const char *p = args + 1;
                while (*p && *p != ']') {
                    p = skip_ws(p);
                    if (*p == ']') break;
                    extract_shard_key_from_where(p, result);
                    if (result->shard_key_found)
                        return; /* found one, stop */
                    p = json_skip_value(p);
                    p = skip_ws(p);
                    if (*p == ',') p++;
                }
            }
        }
        return;
    }

    /* Check operator is "=" */
    const char *name_arr = json_find_key(aexpr, "name");
    if (!name_arr)
        return;

    const char *sval = json_find_key_recursive(name_arr, "sval");
    if (!sval)
        return;

    char *op = json_get_string(sval);
    if (!op || strcmp(op, "=") != 0) {
        free(op);
        return;
    }
    free(op);

    /* Extract column name from lexpr (ColumnRef) */
    const char *lexpr = json_find_key(aexpr, "lexpr");
    if (!lexpr)
        return;

    const char *colref = json_find_key(lexpr, "ColumnRef");
    if (!colref) return;

    const char *fields = json_find_key(colref, "fields");
    if (!fields) return;

    /* Get the column name: find first String.sval inside fields array.
     * fields is like: [{"String":{"sval":"user_id"}}] */
    const char *str_node = json_find_key_recursive(fields, "String");
    if (str_node) {
        const char *sv = json_find_key(str_node, "sval");
        if (sv) {
            free(result->shard_key_column);
            result->shard_key_column = json_get_string(sv);
        }
    }

    if (!result->shard_key_column)
        return;

    /* Extract value from rexpr (A_Const) */
    const char *rexpr = json_find_key(aexpr, "rexpr");
    if (!rexpr)
        return;

    const char *aconst = json_find_key(rexpr, "A_Const");
    if (!aconst)
        return;

    /* Check for integer value */
    const char *ival_obj = json_find_key(aconst, "ival");
    if (ival_obj) {
        const char *ival = json_find_key(ival_obj, "ival");
        if (ival) {
            result->shard_key_int = json_get_int(ival);
            result->shard_key_is_int = true;
            result->shard_key_found = true;
            return;
        }
    }

    /* Check for string value */
    const char *sval_obj = json_find_key(aconst, "sval");
    if (sval_obj) {
        const char *sv = json_find_key(sval_obj, "sval");
        if (sv) {
            result->shard_key_str = json_get_string(sv);
            result->shard_key_is_int = false;
            result->shard_key_found = true;
            return;
        }
    }
}

/*
 * For INSERT, extract shard key from target column list and VALUES.
 */
static void extract_shard_key_from_insert(const char *insert_json,
                                          ParseResult *result,
                                          const char *shard_column)
{
    if (!shard_column)
        return;

    /* Find the column list (cols) */
    const char *cols = json_find_key(insert_json, "cols");
    if (!cols || *cols != '[')
        return;

    /* Find which position the shard column is in */
    int target_pos = -1;
    int pos = 0;

    int name_count;
    const char **names = json_find_all_values(cols, "name", &name_count);
    for (int i = 0; i < name_count; i++) {
        char *name = json_get_string(names[i]);
        if (name && strcmp(name, shard_column) == 0) {
            target_pos = pos;
            free(name);
            break;
        }
        free(name);
        pos++;
    }
    free(names);

    if (target_pos < 0)
        return;

    /* Find the values list */
    const char *select_stmt = json_find_key(insert_json, "selectStmt");
    if (!select_stmt)
        return;

    const char *ss = json_find_key(select_stmt, "SelectStmt");
    if (!ss)
        return;

    const char *values_lists = json_find_key(ss, "valuesLists");
    if (!values_lists)
        return;

    /* Get the first values list */
    const char *first_list = json_find_key_recursive(values_lists, "items");
    if (!first_list || *first_list != '[')
        return;

    /* Skip to the target_pos-th element */
    const char *p = first_list + 1;
    for (int i = 0; i < target_pos && *p && *p != ']'; i++) {
        p = skip_ws(p);
        p = json_skip_value(p);
        p = skip_ws(p);
        if (*p == ',') p++;
    }

    p = skip_ws(p);
    if (*p == ']')
        return;

    /* Extract the value at this position */
    const char *aconst = json_find_key_recursive(p, "A_Const");
    if (!aconst)
        return;

    const char *ival_obj = json_find_key(aconst, "ival");
    if (ival_obj) {
        const char *ival = json_find_key(ival_obj, "ival");
        if (ival) {
            result->shard_key_int = json_get_int(ival);
            result->shard_key_is_int = true;
            result->shard_key_found = true;
            result->shard_key_column = strdup(shard_column);
            return;
        }
    }

    const char *sval_obj = json_find_key(aconst, "sval");
    if (sval_obj) {
        const char *sv = json_find_key(sval_obj, "sval");
        if (sv) {
            result->shard_key_str = json_get_string(sv);
            result->shard_key_is_int = false;
            result->shard_key_found = true;
            result->shard_key_column = strdup(shard_column);
        }
    }
}


int parse_query(const char *sql, ParseResult *result)
{
    memset(result, 0, sizeof(ParseResult));
    result->stmt_type = STMT_UNKNOWN;

    PgQueryParseResult pgr = pg_query_parse(sql);
    if (pgr.error) {
        log_error("parse error: %s (at pos %d)", pgr.error->message, pgr.error->cursorpos);
        pg_query_free_parse_result(pgr);
        return -1;
    }

    result->parse_tree_json = strdup(pgr.parse_tree);

    /* Navigate to stmts[0].stmt */
    const char *stmts = json_find_key(pgr.parse_tree, "stmts");
    if (!stmts || *stmts != '[') {
        pg_query_free_parse_result(pgr);
        return -1;
    }

    /* Skip '[' to first element */
    const char *first_stmt_wrapper = skip_ws(stmts + 1);
    if (*first_stmt_wrapper == ']') {
        pg_query_free_parse_result(pgr);
        return 0; /* empty query */
    }

    const char *stmt_obj = json_find_key(first_stmt_wrapper, "stmt");
    if (!stmt_obj) {
        pg_query_free_parse_result(pgr);
        return -1;
    }

    result->stmt_type = detect_stmt_type(stmt_obj);

    /* Extract table name and shard key based on stmt type */
    switch (result->stmt_type) {
    case STMT_SELECT: {
        const char *ss = json_find_key(stmt_obj, "SelectStmt");
        if (ss) {
            /* fromClause -> first RangeVar */
            const char *from = json_find_key(ss, "fromClause");
            if (from) {
                const char *rv = json_find_key_recursive(from, "RangeVar");
                if (!rv) {
                    /* RangeVar might be at top level of array element */
                    rv = json_find_key_recursive(from, "relname");
                    if (rv) {
                        /* We found relname directly, go up to its parent */
                        free(result->table_name);
                        result->table_name = json_get_string(rv);
                    }
                } else {
                    extract_table_from_rangevar(rv, result);
                }
            }
            /* whereClause */
            const char *where = json_find_key(ss, "whereClause");
            if (where)
                extract_shard_key_from_where(where, result);
        }
        break;
    }

    case STMT_INSERT: {
        const char *is = json_find_key(stmt_obj, "InsertStmt");
        if (is) {
            const char *rel = json_find_key(is, "relation");
            if (rel)
                extract_table_from_rangevar(rel, result);
            /* Note: shard key extraction for INSERT needs the column name
             * from the table rule, which is done at the router level */
        }
        break;
    }

    case STMT_UPDATE: {
        const char *us = json_find_key(stmt_obj, "UpdateStmt");
        if (us) {
            const char *rel = json_find_key(us, "relation");
            if (rel)
                extract_table_from_rangevar(rel, result);
            const char *where = json_find_key(us, "whereClause");
            if (where)
                extract_shard_key_from_where(where, result);
        }
        break;
    }

    case STMT_DELETE: {
        const char *ds = json_find_key(stmt_obj, "DeleteStmt");
        if (ds) {
            const char *rel = json_find_key(ds, "relation");
            if (rel)
                extract_table_from_rangevar(rel, result);
            const char *where = json_find_key(ds, "whereClause");
            if (where)
                extract_shard_key_from_where(where, result);
        }
        break;
    }

    case STMT_DDL: {
        /* Try to find relname anywhere in the statement */
        const char *rv = json_find_key_recursive(stmt_obj, "relname");
        if (rv) {
            result->table_name = json_get_string(rv);
        }
        break;
    }

    default:
        break;
    }

    pg_query_free_parse_result(pgr);
    return 0;
}

/* For INSERT, we need a second pass with the shard column name known */
void parse_extract_insert_shard_key(ParseResult *result, const char *shard_column)
{
    if (result->stmt_type != STMT_INSERT || !result->parse_tree_json || !shard_column)
        return;

    const char *stmts = json_find_key(result->parse_tree_json, "stmts");
    if (!stmts) return;

    const char *first = skip_ws(stmts + 1);
    const char *stmt = json_find_key(first, "stmt");
    if (!stmt) return;

    const char *is = json_find_key(stmt, "InsertStmt");
    if (!is) return;

    extract_shard_key_from_insert(is, result, shard_column);
}

void parse_result_free(ParseResult *result)
{
    free(result->table_name);
    free(result->schema_name);
    free(result->shard_key_column);
    free(result->shard_key_str);
    free(result->parse_tree_json);
    memset(result, 0, sizeof(ParseResult));
}
