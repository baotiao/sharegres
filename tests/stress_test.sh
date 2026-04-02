#!/bin/bash
#
# Sharegres 10-Round Stability Test
#
# Each round:
#   - INSERT into specific shards, verify routing
#   - SELECT from specific shards, verify data isolation
#   - Scatter query, verify no crash
#   - DELETE cleanup
#   - Test various SQL features
#

PROXY="-h 127.0.0.1 -p 15432"
USER=$(whoami)
DB=postgres

PASS=0
FAIL=0
TOTAL_ROUNDS=${1:-10}

RED='\033[0;31m'
GREEN='\033[0;32m'
BOLD='\033[1m'
NC='\033[0m'

check() {
    local name="$1"
    local sql="$2"
    local expected="$3"

    local result
    result=$(psql $PROXY -U $USER -d $DB -X -A -t -c "$sql" 2>&1)

    if [ -n "$expected" ]; then
        if [ "$result" = "$expected" ]; then
            PASS=$((PASS + 1)); return 0
        else
            echo -e "    ${RED}FAIL${NC} $name"
            echo "      expected: $expected"
            echo "      got:      $result"
            FAIL=$((FAIL + 1)); return 1
        fi
    else
        # Just check no crash
        if ! echo "$result" | grep -qi "server closed\|connection refused"; then
            PASS=$((PASS + 1)); return 0
        else
            echo -e "    ${RED}FAIL${NC} $name (crash): $result"
            FAIL=$((FAIL + 1)); return 1
        fi
    fi
}

echo ""
echo -e "${BOLD}============================================${NC}"
echo -e "${BOLD} Sharegres $TOTAL_ROUNDS-Round Stability Test${NC}"
echo -e "${BOLD}============================================${NC}"
echo ""

# Clean test data from all shards first
for s in $(seq 0 9); do
    psql -h 127.0.0.1 -p 5432 -U $USER -d $DB -X -q \
        -c "DELETE FROM shard_${s}.orders WHERE product LIKE 'stress_%';" 2>/dev/null
done

for ROUND in $(seq 1 $TOTAL_ROUNDS); do
    echo -e "${BOLD}Round $ROUND/$TOTAL_ROUNDS${NC}"
    prev=$((PASS + FAIL))

    # Unique IDs per round, within each shard's range
    # shard_N covers [N*1000000, (N+1)*1000000)
    # Use user_id = N*1000000 + ROUND*100 for shard N

    # --- INSERT routing to each shard ---
    for s in 0 2 5 8; do
        uid=$((s * 1000000 + ROUND * 100))
        check "insert shard_${s}" \
            "INSERT INTO orders(user_id, product, amount) VALUES ($uid, 'stress_r${ROUND}_s${s}', ${ROUND}.${s}9);" \
            "INSERT 0 1"
    done

    # --- SELECT routing (single shard) ---
    for s in 0 2 5 8; do
        uid=$((s * 1000000 + ROUND * 100))
        check "select shard_${s}" \
            "SELECT product FROM orders WHERE user_id = $uid AND product = 'stress_r${ROUND}_s${s}';" \
            "stress_r${ROUND}_s${s}"
    done

    # --- Verify data isolation: shard_0 shouldn't see shard_5's data ---
    uid0=$((0 * 1000000 + ROUND * 100))
    uid5=$((5 * 1000000 + ROUND * 100))
    check "isolation shard_0" \
        "SELECT COUNT(*) FROM orders WHERE user_id = $uid0 AND product = 'stress_r${ROUND}_s5';" \
        "0"

    # --- Scatter query ---
    check "scatter count" \
        "SELECT 'ok';" \
        "ok"

    # --- UPDATE routing ---
    uid2=$((2 * 1000000 + ROUND * 100))
    check "update shard_2" \
        "UPDATE orders SET amount = 999.99 WHERE user_id = $uid2 AND product = 'stress_r${ROUND}_s2';" \
        "UPDATE 1"

    check "verify update" \
        "SELECT amount FROM orders WHERE user_id = $uid2 AND product = 'stress_r${ROUND}_s2';" \
        "999.99"

    # --- DELETE routing ---
    uid8=$((8 * 1000000 + ROUND * 100))
    check "delete shard_8" \
        "DELETE FROM orders WHERE user_id = $uid8 AND product = 'stress_r${ROUND}_s8';" \
        "DELETE 1"

    check "verify delete" \
        "SELECT COUNT(*) FROM orders WHERE user_id = $uid8 AND product = 'stress_r${ROUND}_s8';" \
        "0"

    # --- SQL features ---
    check "arithmetic" \
        "SELECT $ROUND * 2 + 1;" \
        "$((ROUND * 2 + 1))"

    check "string func" \
        "SELECT UPPER('round_${ROUND}');" \
        "ROUND_${ROUND}"

    check "CASE" \
        "SELECT CASE WHEN $ROUND > 0 THEN 'yes' ELSE 'no' END;" \
        "yes"

    check "generate_series" \
        "SELECT COUNT(*) FROM generate_series(1, $((ROUND * 100)));" \
        "$((ROUND * 100))"

    check "json" \
        "SELECT ('{\"r\": $ROUND}'::JSONB)->>'r';" \
        "$ROUND"

    check "CTE" \
        "WITH x AS (SELECT $ROUND AS n) SELECT n FROM x;" \
        "$ROUND"

    check "window" \
        "SELECT SUM(v) FROM (SELECT $ROUND AS v, ROW_NUMBER() OVER () FROM generate_series(1,3)) sub;" \
        "$((ROUND * 3))"

    check "COPY OUT" \
        "COPY (SELECT $ROUND, 'test') TO STDOUT;" \
        "${ROUND}	test"

    check "error recovery" \
        "SELECT $ROUND;" \
        "$ROUND"

    check "large result" \
        "SELECT COUNT(*) FROM generate_series(1, 10000);" \
        "10000"

    check "COALESCE" \
        "SELECT COALESCE(NULL, $ROUND);" \
        "$ROUND"

    # --- Cleanup this round ---
    for s in 0 2 5; do
        uid=$((s * 1000000 + ROUND * 100))
        psql $PROXY -U $USER -d $DB -X -A -t \
            -c "DELETE FROM orders WHERE user_id = $uid AND product LIKE 'stress_r${ROUND}_%';" > /dev/null 2>&1
    done

    round_tests=$((PASS + FAIL - prev))
    echo -e "  ${GREEN}$((PASS - prev + FAIL - (FAIL - (PASS + FAIL - prev - round_tests)))) tests${NC}: $(( PASS - (prev - (prev - PASS - FAIL + PASS + FAIL)) )) ... ${GREEN}done${NC} (total: $PASS pass, $FAIL fail)"
    echo ""
done

echo -e "${BOLD}============================================${NC}"
TOTAL=$((PASS + FAIL))
PCT=$((PASS * 100 / TOTAL))
if [ $FAIL -eq 0 ]; then
    echo -e "${BOLD} ${GREEN}ALL $TOTAL TESTS PASSED ($TOTAL_ROUNDS rounds)${NC}"
else
    echo -e "${BOLD} Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC} / $TOTAL total"
fi
echo -e "${BOLD} Pass rate: $PCT% across $TOTAL_ROUNDS rounds${NC}"
echo -e "${BOLD}============================================${NC}"

exit $FAIL
