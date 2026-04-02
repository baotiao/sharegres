#ifndef SHAREGRES_QUERY_PARSER_H
#define SHAREGRES_QUERY_PARSER_H

#include <stdbool.h>
#include <stdint.h>

/* Statement types */
typedef enum {
    STMT_SELECT,
    STMT_INSERT,
    STMT_UPDATE,
    STMT_DELETE,
    STMT_DDL,              /* CREATE, ALTER, DROP, etc. */
    STMT_TRANSACTION,      /* BEGIN, COMMIT, ROLLBACK */
    STMT_SET,              /* SET variable */
    STMT_SHOW,             /* SHOW variable */
    STMT_DISCARD,
    STMT_COPY,
    STMT_UNKNOWN,
} StmtType;

/* Parsed query information */
typedef struct {
    StmtType    stmt_type;
    char       *table_name;         /* primary target table (or NULL) */
    char       *schema_name;        /* schema if specified (or NULL) */

    /* Shard key extraction */
    bool        shard_key_found;
    char       *shard_key_column;   /* column name used in WHERE = */
    int64_t     shard_key_int;      /* integer value (if found) */
    char       *shard_key_str;      /* string value (if found, caller frees) */
    bool        shard_key_is_int;   /* true if integer, false if string */

    /* Raw parse tree (for debugging) */
    char       *parse_tree_json;    /* owned, caller frees via parse_result_free */
} ParseResult;

/* Parse a SQL query and extract routing information. */
int     parse_query(const char *sql, ParseResult *result);

/* For INSERT: extract shard key given the known shard column name. */
void    parse_extract_insert_shard_key(ParseResult *result, const char *shard_column);

/* Free resources in a ParseResult. */
void    parse_result_free(ParseResult *result);

#endif /* SHAREGRES_QUERY_PARSER_H */
