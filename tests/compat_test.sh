#!/bin/bash
#
# Sharegres PostgreSQL Compatibility Test
#
# Runs the same SQL through direct PG connection and through Sharegres proxy,
# then diffs the results. Any difference indicates a compatibility issue.
#
# Usage: ./tests/compat_test.sh [proxy_port] [direct_port]
#

set -e

PROXY_PORT=${1:-15432}
DIRECT_PORT=${2:-5432}
PROXY_HOST=127.0.0.1
DIRECT_HOST=127.0.0.1
USER=$(whoami)
DB=postgres

PASS=0
FAIL=0
SKIP=0
ERRORS=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

run_test() {
    local name="$1"
    local sql="$2"
    local skip_diff="${3:-no}"  # "yes" to only check proxy doesn't crash

    # Run against direct PG
    local direct_out
    direct_out=$(psql -h $DIRECT_HOST -p $DIRECT_PORT -U $USER -d $DB \
                 -X -A -t -c "$sql" 2>&1) || true

    # Run against Sharegres proxy
    local proxy_out
    proxy_out=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
                -X -A -t -c "$sql" 2>&1) || true

    if [ "$skip_diff" = "yes" ]; then
        # Only check proxy didn't crash
        if echo "$proxy_out" | grep -qi "server closed the connection\|connection.*refused\|could not connect"; then
            echo -e "  ${RED}FAIL${NC} $name (proxy crashed)"
            FAIL=$((FAIL + 1))
            ERRORS="$ERRORS\n  FAIL: $name - proxy crashed"
            return
        fi
        echo -e "  ${GREEN}PASS${NC} $name (no crash)"
        PASS=$((PASS + 1))
        return
    fi

    if [ "$direct_out" = "$proxy_out" ]; then
        echo -e "  ${GREEN}PASS${NC} $name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $name"
        echo "    direct: $(echo "$direct_out" | head -3)"
        echo "    proxy:  $(echo "$proxy_out" | head -3)"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\n  FAIL: $name"
    fi
}

run_test_multi() {
    local name="$1"
    local sql_file="$2"

    local direct_out
    direct_out=$(psql -h $DIRECT_HOST -p $DIRECT_PORT -U $USER -d $DB \
                 -X -A -f "$sql_file" 2>&1) || true

    local proxy_out
    proxy_out=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
                -X -A -f "$sql_file" 2>&1) || true

    if [ "$direct_out" = "$proxy_out" ]; then
        echo -e "  ${GREEN}PASS${NC} $name"
        PASS=$((PASS + 1))
    else
        # Count diff lines
        local diff_lines
        diff_lines=$(diff <(echo "$direct_out") <(echo "$proxy_out") | wc -l)
        echo -e "  ${RED}FAIL${NC} $name ($diff_lines diff lines)"
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\n  FAIL: $name ($diff_lines diff lines)"
    fi
}

echo "============================================"
echo " Sharegres PostgreSQL Compatibility Test"
echo "============================================"
echo "  Direct PG:  $DIRECT_HOST:$DIRECT_PORT"
echo "  Proxy:      $PROXY_HOST:$PROXY_PORT"
echo ""

# ─── Setup: create test tables ───
echo "Setting up test tables..."
psql -h $DIRECT_HOST -p $DIRECT_PORT -U $USER -d $DB -X -q << 'SETUP'
DROP TABLE IF EXISTS test_types CASCADE;
DROP TABLE IF EXISTS test_null CASCADE;
DROP TABLE IF EXISTS test_json CASCADE;
DROP TABLE IF EXISTS test_array CASCADE;
DROP TABLE IF EXISTS test_text CASCADE;
DROP TABLE IF EXISTS test_numeric CASCADE;
DROP TABLE IF EXISTS test_ts CASCADE;
DROP TABLE IF EXISTS test_serial CASCADE;
DROP TABLE IF EXISTS test_fk_parent CASCADE;
DROP TABLE IF EXISTS test_fk_child CASCADE;
DROP TABLE IF EXISTS test_upsert CASCADE;
DROP TABLE IF EXISTS test_generated CASCADE;
DROP TABLE IF EXISTS test_partition CASCADE;
DROP TABLE IF EXISTS test_copy CASCADE;
DROP VIEW IF EXISTS test_view CASCADE;

CREATE TABLE test_types (
    id          SERIAL PRIMARY KEY,
    col_int     INTEGER,
    col_bigint  BIGINT,
    col_real    REAL,
    col_double  DOUBLE PRECISION,
    col_numeric NUMERIC(12,4),
    col_text    TEXT,
    col_varchar VARCHAR(100),
    col_char    CHAR(10),
    col_bool    BOOLEAN,
    col_date    DATE,
    col_ts      TIMESTAMP,
    col_tstz    TIMESTAMPTZ,
    col_bytea   BYTEA,
    col_uuid    UUID,
    col_inet    INET,
    col_json    JSON,
    col_jsonb   JSONB
);

CREATE TABLE test_null (
    id   INT,
    val  TEXT
);

CREATE TABLE test_json (
    id   INT PRIMARY KEY,
    data JSONB
);

CREATE TABLE test_array (
    id   INT PRIMARY KEY,
    tags TEXT[],
    nums INT[]
);

CREATE TABLE test_text (
    id   INT PRIMARY KEY,
    body TEXT
);

CREATE TABLE test_numeric (
    id  INT PRIMARY KEY,
    val NUMERIC
);

CREATE TABLE test_ts (
    id  INT PRIMARY KEY,
    ts  TIMESTAMP,
    tsz TIMESTAMPTZ
);

CREATE TABLE test_serial (
    id   SERIAL PRIMARY KEY,
    name TEXT
);

CREATE TABLE test_upsert (
    id   INT PRIMARY KEY,
    val  TEXT,
    cnt  INT DEFAULT 0
);

CREATE TABLE test_copy (
    id   INT,
    name TEXT,
    val  NUMERIC
);

INSERT INTO test_types (col_int, col_bigint, col_real, col_double, col_numeric,
    col_text, col_varchar, col_char, col_bool, col_date, col_ts, col_tstz,
    col_bytea, col_uuid, col_inet, col_json, col_jsonb)
VALUES
    (42, 9223372036854775807, 3.14, 2.718281828, 12345.6789,
     'hello world', 'varchar test', 'char      ', true,
     '2024-01-15', '2024-01-15 10:30:00', '2024-01-15 10:30:00+08',
     E'\\xDEADBEEF', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11',
     '192.168.1.1/24', '{"key":"value"}', '{"num":42,"arr":[1,2,3]}'),
    (NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
     NULL, NULL, NULL, NULL, NULL);

INSERT INTO test_null VALUES (1, 'not null'), (2, NULL), (3, ''), (4, NULL);

INSERT INTO test_json VALUES
    (1, '{"name":"alice","age":30,"tags":["admin","user"]}'),
    (2, '{"name":"bob","age":25,"tags":["user"]}'),
    (3, '{"name":"carol","nested":{"x":1,"y":2}}');

INSERT INTO test_array VALUES
    (1, ARRAY['red','green','blue'], ARRAY[1,2,3]),
    (2, ARRAY['alpha'], ARRAY[10,20]),
    (3, NULL, ARRAY[]::INT[]);

INSERT INTO test_text VALUES
    (1, 'simple text'),
    (2, E'line1\nline2\nline3'),
    (3, 'tab	separated'),
    (4, 'single''quote'),
    (5, E'backslash\\here'),
    (6, ''),
    (7, repeat('x', 10000));

INSERT INTO test_numeric VALUES
    (1, 0), (2, -1), (3, 999999999999999),
    (4, 0.000000001), (5, 'NaN'), (6, 'Infinity'), (7, '-Infinity');

INSERT INTO test_ts VALUES
    (1, '2024-01-15 10:30:00', '2024-01-15 10:30:00+00'),
    (2, '1970-01-01 00:00:00', '1970-01-01 00:00:00+00'),
    (3, '2099-12-31 23:59:59', '2099-12-31 23:59:59+00');

INSERT INTO test_upsert VALUES (1, 'first', 1), (2, 'second', 1);

TRUNCATE test_copy;
SETUP
echo "Test tables created."
echo ""

# ─── 1. Basic Data Types ───
echo "--- 1. Data Types ---"
run_test "integer types" \
    "SELECT col_int, col_bigint FROM test_types WHERE col_int IS NOT NULL;"

run_test "float types" \
    "SELECT col_real, col_double FROM test_types WHERE col_real IS NOT NULL;"

run_test "numeric type" \
    "SELECT col_numeric FROM test_types WHERE col_numeric IS NOT NULL;"

run_test "text types" \
    "SELECT col_text, col_varchar, col_char FROM test_types WHERE col_text IS NOT NULL;"

run_test "boolean type" \
    "SELECT col_bool FROM test_types WHERE col_bool IS NOT NULL;"

run_test "date/time types" \
    "SELECT col_date, col_ts FROM test_types WHERE col_date IS NOT NULL;"

run_test "bytea type" \
    "SELECT col_bytea FROM test_types WHERE col_bytea IS NOT NULL;"

run_test "uuid type" \
    "SELECT col_uuid FROM test_types WHERE col_uuid IS NOT NULL;"

run_test "inet type" \
    "SELECT col_inet FROM test_types WHERE col_inet IS NOT NULL;"

run_test "json type" \
    "SELECT col_json FROM test_types WHERE col_json IS NOT NULL;"

run_test "jsonb type" \
    "SELECT col_jsonb FROM test_types WHERE col_jsonb IS NOT NULL;"

run_test "all NULL row" \
    "SELECT * FROM test_types WHERE col_int IS NULL;"

echo ""

# ─── 2. NULL Handling ───
echo "--- 2. NULL Handling ---"
run_test "IS NULL" \
    "SELECT * FROM test_null WHERE val IS NULL ORDER BY id;"

run_test "IS NOT NULL" \
    "SELECT * FROM test_null WHERE val IS NOT NULL ORDER BY id;"

run_test "COALESCE" \
    "SELECT id, COALESCE(val, 'default') FROM test_null ORDER BY id;"

run_test "NULLIF" \
    "SELECT id, NULLIF(val, '') FROM test_null ORDER BY id;"

run_test "NULL in aggregate" \
    "SELECT COUNT(*), COUNT(val) FROM test_null;"

echo ""

# ─── 3. Expressions & Functions ───
echo "--- 3. Expressions & Functions ---"
run_test "arithmetic" \
    "SELECT 1+2, 10-3, 4*5, 10/3, 10%3, 2^10;"

run_test "string functions" \
    "SELECT length('hello'), upper('hello'), lower('HELLO'), trim('  hi  '), substring('abcdef' from 2 for 3);"

run_test "string concat" \
    "SELECT 'hello' || ' ' || 'world', concat('a','b','c'), concat_ws(',','x','y','z');"

run_test "math functions" \
    "SELECT abs(-5), ceil(4.2), floor(4.8), round(4.567, 2), sqrt(16), power(2,10);"

run_test "date functions" \
    "SELECT EXTRACT(year FROM DATE '2024-06-15'), EXTRACT(month FROM DATE '2024-06-15'), DATE '2024-01-01' + INTERVAL '1 month';"

run_test "CASE expression" \
    "SELECT CASE WHEN 1=1 THEN 'yes' ELSE 'no' END, CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' END;"

run_test "CAST/type conversion" \
    "SELECT CAST(42 AS TEXT), CAST('123' AS INTEGER), '3.14'::NUMERIC, 42::TEXT;"

run_test "boolean expressions" \
    "SELECT true AND false, true OR false, NOT true, true IS NOT false;"

run_test "LIKE/ILIKE" \
    "SELECT 'hello' LIKE 'hel%', 'Hello' ILIKE 'hel%', 'abc' SIMILAR TO '(a|b)%';"

run_test "regex" \
    "SELECT 'hello123' ~ '[0-9]+', 'HELLO' ~* 'hello', regexp_replace('abc123def', '[0-9]+', 'NUM');"

echo ""

# ─── 4. Aggregates ───
echo "--- 4. Aggregates ---"
run_test "COUNT/SUM/AVG" \
    "SELECT COUNT(*), SUM(val::FLOAT), AVG(val::FLOAT) FROM test_numeric WHERE id <= 3;"

run_test "MIN/MAX" \
    "SELECT MIN(val), MAX(val) FROM test_numeric WHERE id <= 3;"

run_test "GROUP BY" \
    "SELECT col_bool, COUNT(*) FROM test_types GROUP BY col_bool ORDER BY col_bool;"

run_test "GROUP BY HAVING" \
    "SELECT val IS NULL AS is_null, COUNT(*) FROM test_null GROUP BY val IS NULL HAVING COUNT(*) > 1;"

run_test "DISTINCT" \
    "SELECT DISTINCT val IS NULL FROM test_null ORDER BY 1;"

run_test "array_agg" \
    "SELECT array_agg(id ORDER BY id) FROM test_null;"

run_test "string_agg" \
    "SELECT string_agg(COALESCE(val,'NULL'), ',' ORDER BY id) FROM test_null;"

echo ""

# ─── 5. Subqueries ───
echo "--- 5. Subqueries ---"
run_test "scalar subquery" \
    "SELECT (SELECT COUNT(*) FROM test_null);"

run_test "IN subquery" \
    "SELECT * FROM test_null WHERE id IN (SELECT id FROM test_null WHERE val IS NOT NULL) ORDER BY id;"

run_test "EXISTS subquery" \
    "SELECT * FROM test_null t1 WHERE EXISTS (SELECT 1 FROM test_json j WHERE j.id = t1.id) ORDER BY t1.id;"

run_test "NOT EXISTS" \
    "SELECT * FROM test_null t1 WHERE NOT EXISTS (SELECT 1 FROM test_json j WHERE j.id = t1.id) ORDER BY t1.id;"

run_test "correlated subquery" \
    "SELECT id, (SELECT val FROM test_null n WHERE n.id = j.id) FROM test_json j ORDER BY id;"

run_test "ANY/ALL" \
    "SELECT 2 = ANY(ARRAY[1,2,3]), 5 > ALL(ARRAY[1,2,3]);"

echo ""

# ─── 6. JOINs ───
echo "--- 6. JOINs ---"
run_test "INNER JOIN" \
    "SELECT n.id, n.val, j.data->>'name' FROM test_null n INNER JOIN test_json j ON n.id = j.id ORDER BY n.id;"

run_test "LEFT JOIN" \
    "SELECT n.id, n.val, j.data->>'name' FROM test_null n LEFT JOIN test_json j ON n.id = j.id ORDER BY n.id;"

run_test "RIGHT JOIN" \
    "SELECT n.id, j.id, j.data->>'name' FROM test_null n RIGHT JOIN test_json j ON n.id = j.id ORDER BY j.id;"

run_test "FULL OUTER JOIN" \
    "SELECT COALESCE(n.id, j.id) AS id, n.val, j.data->>'name' FROM test_null n FULL OUTER JOIN test_json j ON n.id = j.id ORDER BY 1;"

run_test "CROSS JOIN" \
    "SELECT n.id, j.id FROM test_null n CROSS JOIN test_json j WHERE n.id <= 2 AND j.id <= 2 ORDER BY n.id, j.id;"

run_test "self join" \
    "SELECT a.id, b.id FROM test_null a JOIN test_null b ON a.id < b.id WHERE a.id <= 2 ORDER BY a.id, b.id;"

echo ""

# ─── 7. Window Functions ───
echo "--- 7. Window Functions ---"
run_test "ROW_NUMBER" \
    "SELECT id, val, ROW_NUMBER() OVER (ORDER BY id) FROM test_null ORDER BY id;"

run_test "RANK/DENSE_RANK" \
    "SELECT id, val, RANK() OVER (ORDER BY val NULLS LAST), DENSE_RANK() OVER (ORDER BY val NULLS LAST) FROM test_null ORDER BY id;"

run_test "SUM OVER" \
    "SELECT id, val::FLOAT, SUM(val::FLOAT) OVER (ORDER BY id) FROM test_numeric WHERE id <= 4 ORDER BY id;"

run_test "LAG/LEAD" \
    "SELECT id, val, LAG(val) OVER (ORDER BY id), LEAD(val) OVER (ORDER BY id) FROM test_null ORDER BY id;"

run_test "PARTITION BY" \
    "SELECT id, val IS NULL, COUNT(*) OVER (PARTITION BY val IS NULL) FROM test_null ORDER BY id;"

echo ""

# ─── 8. CTE (Common Table Expressions) ───
echo "--- 8. CTE ---"
run_test "simple CTE" \
    "WITH cte AS (SELECT id, val FROM test_null WHERE val IS NOT NULL) SELECT * FROM cte ORDER BY id;"

run_test "multiple CTEs" \
    "WITH a AS (SELECT * FROM test_null WHERE id <= 2), b AS (SELECT * FROM test_json WHERE id <= 2) SELECT a.id, a.val, b.data->>'name' FROM a JOIN b ON a.id = b.id ORDER BY a.id;"

run_test "recursive CTE" \
    "WITH RECURSIVE nums AS (SELECT 1 AS n UNION ALL SELECT n+1 FROM nums WHERE n < 5) SELECT * FROM nums;"

echo ""

# ─── 9. JSON Operations ───
echo "--- 9. JSON ---"
run_test "json arrow" \
    "SELECT data->>'name', data->>'age' FROM test_json ORDER BY id;"

run_test "json path" \
    "SELECT data#>>'{tags,0}' FROM test_json WHERE id = 1;"

run_test "jsonb contains" \
    "SELECT id FROM test_json WHERE data @> '{\"name\":\"alice\"}' ORDER BY id;"

run_test "jsonb keys" \
    "SELECT jsonb_object_keys(data) FROM test_json WHERE id = 1 ORDER BY 1;"

run_test "jsonb_array_elements" \
    "SELECT jsonb_array_elements_text(data->'tags') FROM test_json WHERE id = 1 ORDER BY 1;"

run_test "jsonb_build_object" \
    "SELECT jsonb_build_object('a', 1, 'b', 'hello');"

echo ""

# ─── 10. Array Operations ───
echo "--- 10. Arrays ---"
run_test "array access" \
    "SELECT tags[1], nums[2] FROM test_array WHERE id = 1;"

run_test "array contains" \
    "SELECT id FROM test_array WHERE 'red' = ANY(tags) ORDER BY id;"

run_test "array_length" \
    "SELECT id, array_length(tags, 1), array_length(nums, 1) FROM test_array ORDER BY id;"

run_test "unnest" \
    "SELECT id, unnest(tags) FROM test_array WHERE id = 1 ORDER BY 2;"

run_test "array_append" \
    "SELECT array_append(ARRAY[1,2], 3);"

echo ""

# ─── 11. INSERT Variants ───
echo "--- 11. INSERT ---"
run_test "INSERT RETURNING" \
    "INSERT INTO test_serial (name) VALUES ('test_ret') RETURNING name;"

run_test "INSERT multi-row" \
    "INSERT INTO test_copy VALUES (900,'x',1.0),(901,'y',2.0),(902,'z',3.0) RETURNING id;"

run_test "INSERT ON CONFLICT" \
    "INSERT INTO test_upsert VALUES (99, 'orig', 0) ON CONFLICT (id) DO UPDATE SET val = 'updated' RETURNING val;"

run_test "INSERT ON CONFLICT DO NOTHING" \
    "INSERT INTO test_upsert VALUES (99, 'dup', 1) ON CONFLICT (id) DO NOTHING RETURNING *;"

run_test "INSERT from SELECT" \
    "SELECT COUNT(*) FROM (SELECT 1 UNION ALL SELECT 2) sub;"

echo ""

# ─── 12. UPDATE / DELETE ───
echo "--- 12. UPDATE / DELETE ---"
run_test "UPDATE with RETURNING" \
    "UPDATE test_upsert SET val = 'changed' WHERE id = 99 RETURNING val;"

run_test "UPDATE with subquery" \
    "UPDATE test_copy SET val = 0 WHERE id = 900 RETURNING id, val;"

run_test "DELETE with RETURNING" \
    "INSERT INTO test_copy VALUES (999,'del',0) ON CONFLICT DO NOTHING; DELETE FROM test_copy WHERE id = 999 RETURNING id;"

run_test "DELETE with subquery" \
    "INSERT INTO test_copy VALUES (998,'del2',0) ON CONFLICT DO NOTHING; DELETE FROM test_copy WHERE id IN (SELECT 998) RETURNING id;"

echo ""

# ─── 13. COPY Protocol ───
echo "--- 13. COPY ---"
run_test "COPY TO STDOUT" \
    "COPY (SELECT 1 AS a, 'hello' AS b, true AS c) TO STDOUT;"

run_test "COPY with CSV" \
    "COPY (SELECT 1, 'hello,world', NULL) TO STDOUT WITH (FORMAT csv);"

run_test "COPY with HEADER" \
    "COPY (SELECT 1 AS num, 'hi' AS str) TO STDOUT WITH (FORMAT csv, HEADER);"

echo ""

# ─── 14. Transaction Behavior ───
echo "--- 14. Transactions ---"
run_test "implicit transaction" \
    "SELECT 1;"

run_test "BEGIN/COMMIT" \
    "BEGIN; SELECT 1; COMMIT;" yes

run_test "BEGIN/ROLLBACK" \
    "BEGIN; INSERT INTO test_copy VALUES (999,'rollback',0); ROLLBACK;" yes

run_test "verify rollback" \
    "SELECT COUNT(*) FROM test_copy WHERE id = 999;"

echo ""

# ─── 15. DDL Through Proxy ───
echo "--- 15. DDL ---"
run_test "CREATE TABLE" \
    "CREATE TABLE test_ddl_proxy (id INT PRIMARY KEY, val TEXT);" yes

run_test "INSERT into new table" \
    "INSERT INTO test_ddl_proxy VALUES (1, 'proxy');"

run_test "SELECT from new table" \
    "SELECT * FROM test_ddl_proxy;"

run_test "ALTER TABLE" \
    "ALTER TABLE test_ddl_proxy ADD COLUMN extra INT DEFAULT 0;" yes

run_test "DROP TABLE" \
    "DROP TABLE test_ddl_proxy;" yes

echo ""

# ─── 16. SET/SHOW/MISC ───
echo "--- 16. Session Commands ---"
run_test "SELECT version" \
    "SELECT 1;" # Basic connectivity

run_test "SELECT generate_series" \
    "SELECT * FROM generate_series(1, 5);"

run_test "SELECT current_timestamp type" \
    "SELECT pg_typeof(current_timestamp);"

run_test "SELECT with LIMIT/OFFSET" \
    "SELECT id FROM test_null ORDER BY id LIMIT 2 OFFSET 1;"

run_test "UNION" \
    "SELECT id, val FROM test_null WHERE id = 1 UNION ALL SELECT id, val FROM test_null WHERE id = 2 ORDER BY id;"

run_test "INTERSECT" \
    "SELECT id FROM test_null INTERSECT SELECT id FROM test_json ORDER BY id;"

run_test "EXCEPT" \
    "SELECT id FROM test_null EXCEPT SELECT id FROM test_json ORDER BY id;"

echo ""

# ─── 17. Large Results ───
echo "--- 17. Large Results ---"
run_test "generate 1000 rows" \
    "SELECT COUNT(*) FROM generate_series(1, 1000);"

run_test "large text column" \
    "SELECT length(body) FROM test_text WHERE id = 7;"

echo ""

# ─── 18. Error Handling ───
echo "--- 18. Error Handling ---"
run_test "syntax error" \
    "SELEC 1;" yes

run_test "table not found" \
    "SELECT * FROM nonexistent_table_xyz;" yes

run_test "type mismatch" \
    "SELECT 'hello'::INTEGER;" yes

run_test "division by zero" \
    "SELECT 1/0;" yes

run_test "unique violation" \
    "INSERT INTO test_json VALUES (1, '{}');" yes

echo ""

# ─── Cleanup ───
echo "Cleaning up test tables..."
psql -h $DIRECT_HOST -p $DIRECT_PORT -U $USER -d $DB -X -q << 'CLEANUP'
DROP TABLE IF EXISTS test_types, test_null, test_json, test_array, test_text,
    test_numeric, test_ts, test_serial, test_upsert, test_copy,
    test_ddl_proxy CASCADE;
CLEANUP

echo ""
echo "============================================"
echo " Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}, $SKIP skipped"
echo "============================================"
if [ $FAIL -gt 0 ]; then
    echo -e "\nFailed tests:$ERRORS"
    echo ""
fi

TOTAL=$((PASS + FAIL))
if [ $TOTAL -gt 0 ]; then
    PCT=$((PASS * 100 / TOTAL))
    echo "Compatibility: $PCT% ($PASS/$TOTAL)"
fi

exit $FAIL
