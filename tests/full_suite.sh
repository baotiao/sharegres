#!/bin/bash
#
# Sharegres Full Test Suite
#
# 1. pgbench (select-only, tpcb read-write)
# 2. sysbench OLTP (read-only, read-write, write-only, point-select)
# 3. Correctness: data integrity checks
# 4. Crash recovery: kill sharegres, restart, verify data
# 5. Concurrent stress: parallel psql sessions
#
# Usage: ./tests/full_suite.sh [duration_per_test_seconds]
#

set -o pipefail

PROXY_HOST=127.0.0.1
PROXY_PORT=15432
DIRECT_PORT=5432
USER=$(whoami)
DB=postgres
LOGDIR="/tmp/sharegres_cluster/full_suite_logs"
DURATION=${1:-60}  # seconds per test phase
SHAREGRES_BIN="./bin/sharegres"
SHAREGRES_CONF="/tmp/sharegres_cluster/sharegres_16.cnf"
SHAREGRES_PID_FILE="/tmp/sharegres_cluster/sharegres.pid"
NUM_SHARDS=16

PGBENCH=$(which pgbench)
SYSBENCH=$(which sysbench)

PASS=0
FAIL=0

mkdir -p "$LOGDIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log() { echo -e "${CYAN}[$(date +%H:%M:%S)]${NC} $*" | tee -a "$LOGDIR/suite.log"; }
pass() { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $*" | tee -a "$LOGDIR/suite.log"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $*" | tee -a "$LOGDIR/suite.log"; }

health_check() {
    local r=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t -c "SELECT 1;" 2>&1)
    [ "$r" = "1" ]
}

start_sharegres() {
    pkill -f "sharegres.*$PROXY_PORT" 2>/dev/null; sleep 1
    $SHAREGRES_BIN -c "$SHAREGRES_CONF" > /tmp/sharegres_cluster/sharegres.log 2>&1 &
    echo $! > "$SHAREGRES_PID_FILE"
    sleep 1
    health_check
}

get_sharegres_pid() {
    cat "$SHAREGRES_PID_FILE" 2>/dev/null
}

shard_row_count() {
    local table=$1
    local total=0
    for i in $(seq 0 $((NUM_SHARDS-1))); do
        local c=$(psql -h $PROXY_HOST -p $DIRECT_PORT -U $USER -d $DB -X -A -t \
            -c "SELECT COUNT(*) FROM shard_${i}.${table};" 2>/dev/null)
        total=$((total + c))
    done
    echo $total
}

echo ""
echo -e "${BOLD}================================================================${NC}"
echo -e "${BOLD}  Sharegres Full Test Suite (16 shards, ${DURATION}s per phase)${NC}"
echo -e "${BOLD}================================================================${NC}"
echo ""

health_check || { log "FATAL: proxy not responding"; exit 1; }

########################################
# Phase 1: pgbench
########################################
log ""
log "${BOLD}=== Phase 1: pgbench ===${NC}"

# 1a. pgbench select-only
log "1a. pgbench SELECT-only (${DURATION}s, 8 clients)"
LF="$LOGDIR/pgbench_select.log"
$PGBENCH -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
    -M simple -c 8 -j 4 -T $DURATION -S -P 10 > "$LF" 2>&1
RC=$?
TPS=$(grep "^tps = " "$LF" | tail -1 | awk '{print $3}')
FAILED=$(grep "number of failed" "$LF" | awk -F'[:(]' '{print $2}' | tr -d ' ')
if [ $RC -eq 0 ] && [ "${FAILED:-1}" = "0" ]; then
    pass "pgbench SELECT: ${TPS} TPS, 0 failed"
else
    fail "pgbench SELECT: rc=$RC, failed=$FAILED"
fi

# 1b. pgbench TPC-B (read-write)
log "1b. pgbench TPC-B read-write (${DURATION}s, 4 clients)"
LF="$LOGDIR/pgbench_tpcb.log"
$PGBENCH -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
    -M simple -c 4 -j 2 -T $DURATION -P 10 > "$LF" 2>&1
RC=$?
TPS=$(grep "^tps = " "$LF" | tail -1 | awk '{print $3}')
FAILED=$(grep "number of failed" "$LF" | awk -F'[:(]' '{print $2}' | tr -d ' ')
if [ $RC -eq 0 ] && [ "${FAILED:-1}" = "0" ]; then
    pass "pgbench TPC-B: ${TPS} TPS, 0 failed"
else
    fail "pgbench TPC-B: rc=$RC, failed=$FAILED"
fi

# 1c. pgbench high concurrency
log "1c. pgbench SELECT (${DURATION}s, 16 clients)"
LF="$LOGDIR/pgbench_16c.log"
$PGBENCH -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
    -M simple -c 16 -j 4 -T $DURATION -S -P 10 > "$LF" 2>&1
RC=$?
TPS=$(grep "^tps = " "$LF" | tail -1 | awk '{print $3}')
FAILED=$(grep "number of failed" "$LF" | awk -F'[:(]' '{print $2}' | tr -d ' ')
if [ $RC -eq 0 ] && [ "${FAILED:-1}" = "0" ]; then
    pass "pgbench 16-client: ${TPS} TPS, 0 failed"
else
    fail "pgbench 16-client: rc=$RC, failed=$FAILED"
fi

########################################
# Phase 2: sysbench OLTP (via direct PG, since it uses extended protocol)
########################################
log ""
log "${BOLD}=== Phase 2: sysbench OLTP ===${NC}"

# Initialize sysbench tables
log "Initializing sysbench tables..."
$SYSBENCH /usr/share/sysbench/oltp_read_write.lua \
    --db-driver=pgsql --pgsql-host=$PROXY_HOST --pgsql-port=$DIRECT_PORT \
    --pgsql-user=$USER --pgsql-db=$DB \
    --tables=4 --table-size=50000 \
    prepare > "$LOGDIR/sysbench_prepare.log" 2>&1
log "  sysbench tables ready"

for workload in oltp_point_select oltp_read_only oltp_read_write oltp_write_only; do
    log "2. sysbench $workload (${DURATION}s, 4 threads)"
    LF="$LOGDIR/sysbench_${workload}.log"
    $SYSBENCH /usr/share/sysbench/${workload}.lua \
        --db-driver=pgsql --pgsql-host=$PROXY_HOST --pgsql-port=$DIRECT_PORT \
        --pgsql-user=$USER --pgsql-db=$DB \
        --tables=4 --table-size=50000 \
        --threads=4 --time=$DURATION \
        --report-interval=10 \
        run > "$LF" 2>&1
    RC=$?
    TXN=$(grep "transactions:" "$LF" | awk '{print $2}')
    TPS=$(grep "transactions:" "$LF" | awk -F'(' '{print $2}' | awk '{print $1}')
    if [ $RC -eq 0 ]; then
        pass "sysbench $workload: $TXN txns (${TPS} TPS)"
    else
        fail "sysbench $workload: rc=$RC"
    fi
done

# Cleanup sysbench
$SYSBENCH /usr/share/sysbench/oltp_read_write.lua \
    --db-driver=pgsql --pgsql-host=$PROXY_HOST --pgsql-port=$DIRECT_PORT \
    --pgsql-user=$USER --pgsql-db=$DB \
    --tables=4 cleanup > /dev/null 2>&1

########################################
# Phase 3: Sharded workload stress test
########################################
log ""
log "${BOLD}=== Phase 3: Sharded Workload (through proxy) ===${NC}"

# 3a. Heavy INSERT across 16 shards
log "3a. Bulk INSERT across 16 shards (${DURATION}s)"
LF="$LOGDIR/shard_insert.log"
END_TS=$(($(date +%s) + DURATION))
OPS=0; ERRS=0
while [ $(date +%s) -lt $END_TS ]; do
    for s in $(seq 0 15); do
        uid=$((s * 1000000 + RANDOM % 1000000))
        psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
            -c "INSERT INTO orders(user_id,product,amount) VALUES($uid,'p$((RANDOM%100))',$((RANDOM%9999)).$((RANDOM%99)));" \
            > /dev/null 2>&1 && OPS=$((OPS+1)) || ERRS=$((ERRS+1))
    done
done
echo "ops=$OPS errors=$ERRS" >> "$LF"
if [ $ERRS -eq 0 ]; then
    pass "shard INSERT: $OPS ops, 0 errors"
else
    fail "shard INSERT: $OPS ops, $ERRS errors"
fi

# 3b. Mixed read-write across shards
log "3b. Mixed read-write (${DURATION}s)"
LF="$LOGDIR/shard_mixed.log"
END_TS=$(($(date +%s) + DURATION))
OPS=0; ERRS=0
while [ $(date +%s) -lt $END_TS ]; do
    s=$((RANDOM % 16))
    uid=$((s * 1000000 + RANDOM % 1000000))

    # INSERT
    psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
        -c "INSERT INTO orders(user_id,product,amount) VALUES($uid,'mix',$((RANDOM%100)));" \
        > /dev/null 2>&1 && OPS=$((OPS+1)) || ERRS=$((ERRS+1))

    # SELECT single shard
    psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
        -c "SELECT COUNT(*) FROM orders WHERE user_id=$uid;" \
        > /dev/null 2>&1 && OPS=$((OPS+1)) || ERRS=$((ERRS+1))

    # UPDATE with AND
    psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
        -c "UPDATE orders SET amount=0 WHERE user_id=$uid AND product='mix';" \
        > /dev/null 2>&1 && OPS=$((OPS+1)) || ERRS=$((ERRS+1))

    # Scatter
    psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
        -c "SELECT COUNT(*) FROM orders;" > /dev/null 2>&1 && OPS=$((OPS+1)) || ERRS=$((ERRS+1))
done
echo "ops=$OPS errors=$ERRS" >> "$LF"
if [ $ERRS -eq 0 ]; then
    pass "shard mixed: $OPS ops, 0 errors"
else
    fail "shard mixed: $OPS ops, $ERRS errors"
fi

# 3c. Parallel psql sessions (concurrent connections)
log "3c. Parallel sessions (8 concurrent psql, ${DURATION}s each)"
LF="$LOGDIR/parallel.log"
PIDS=""
for j in $(seq 1 8); do
    (
        END_TS=$(($(date +%s) + DURATION))
        ops=0; errs=0
        while [ $(date +%s) -lt $END_TS ]; do
            s=$((RANDOM % 16))
            uid=$((s * 1000000 + RANDOM % 1000000))
            psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
                -c "SELECT COUNT(*) FROM orders WHERE user_id=$uid;" \
                > /dev/null 2>&1 && ops=$((ops+1)) || errs=$((errs+1))
        done
        echo "worker=$j ops=$ops errs=$errs" >> "$LF"
    ) &
    PIDS="$PIDS $!"
done
wait $PIDS
TOTAL_OPS=$(awk '{s+=$2} END{print s+0}' FS='=' "$LF" 2>/dev/null)
TOTAL_ERRS=$(awk '/errs/{split($3,a,"=");s+=a[2]} END{print s+0}' "$LF" 2>/dev/null)
# Simple check
if health_check; then
    pass "parallel sessions: proxy survived 8 concurrent workers"
else
    fail "parallel sessions: proxy died"
fi

########################################
# Phase 4: Data Integrity
########################################
log ""
log "${BOLD}=== Phase 4: Data Integrity ===${NC}"

# 4a. Compare scatter count vs direct sum
log "4a. Row count integrity"
SCATTER_RAW=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELECT COUNT(*) FROM orders;" 2>/dev/null)
SCATTER_SUM=$(echo "$SCATTER_RAW" | awk '{s+=$1} END{print s}')
DIRECT_SUM=$(shard_row_count orders)
if [ "$SCATTER_SUM" = "$DIRECT_SUM" ]; then
    pass "row count: scatter=$SCATTER_SUM == direct=$DIRECT_SUM"
else
    fail "row count: scatter=$SCATTER_SUM != direct=$DIRECT_SUM"
fi

# 4b. Insert-read-verify
log "4b. Insert-read-verify across all 16 shards"
INTEGRITY_OK=true
for s in $(seq 0 15); do
    uid=$((s * 1000000 + 999999))
    psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
        -c "INSERT INTO orders(user_id,product,amount) VALUES($uid,'integ_v2_$s',${s}.99);" > /dev/null 2>&1
    GOT=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
        -c "SELECT amount FROM orders WHERE user_id=$uid AND product='integ_v2_$s';" 2>/dev/null)
    EXPECTED="${s}.99"
    if [ "$GOT" != "$EXPECTED" ]; then
        fail "integrity shard_$s: expected=$EXPECTED got=$GOT"
        INTEGRITY_OK=false
    fi
    # Verify it's in the correct schema
    DIRECT=$(psql -h $PROXY_HOST -p $DIRECT_PORT -U $USER -d $DB -X -A -t \
        -c "SELECT amount FROM shard_${s}.orders WHERE user_id=$uid AND product='integ_v2_$s' LIMIT 1;" 2>/dev/null)
    if [ "$DIRECT" != "$EXPECTED" ]; then
        fail "integrity direct shard_$s: expected=$EXPECTED got=$DIRECT"
        INTEGRITY_OK=false
    fi
done
if $INTEGRITY_OK; then
    pass "insert-read-verify: all 16 shards correct"
fi

# 4c. Data isolation: shard N should NOT have shard M's data
log "4c. Data isolation"
ISOLATION_OK=true
for s in $(seq 0 15); do
    uid=$((s * 1000000 + 999999))
    wrong_shard=$(( (s + 1) % NUM_SHARDS ))
    LEAK=$(psql -h $PROXY_HOST -p $DIRECT_PORT -U $USER -d $DB -X -A -t \
        -c "SELECT COUNT(*) FROM shard_${wrong_shard}.orders WHERE user_id=$uid AND product='integ_v2_$s';" 2>/dev/null)
    if [ "${LEAK:-0}" != "0" ]; then
        fail "isolation: shard_$s data leaked to shard_$wrong_shard"
        ISOLATION_OK=false
    fi
done
if $ISOLATION_OK; then
    pass "data isolation: no cross-shard leaks"
fi

########################################
# Phase 5: Crash Recovery
########################################
log ""
log "${BOLD}=== Phase 5: Crash Recovery ===${NC}"

# 5a. SIGKILL sharegres during load, restart, verify
log "5a. SIGKILL during load"
# Start background load
(
    for i in $(seq 1 200); do
        uid=$((RANDOM % 16 * 1000000 + RANDOM % 1000000))
        psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
            -c "INSERT INTO orders(user_id,product,amount) VALUES($uid,'crash_test',$i);" > /dev/null 2>&1
    done
) &
LOAD_PID=$!

sleep 2

# Count rows before kill
BEFORE=$(shard_row_count orders)

# SIGKILL the proxy
SG_PID=$(get_sharegres_pid)
log "  Killing sharegres pid=$SG_PID with SIGKILL"
kill -9 $SG_PID 2>/dev/null
wait $LOAD_PID 2>/dev/null

sleep 1

# Verify proxy is dead
if kill -0 $SG_PID 2>/dev/null; then
    fail "crash: sharegres still alive after SIGKILL"
else
    pass "crash: sharegres killed successfully"
fi

# Restart
log "  Restarting sharegres..."
start_sharegres
if health_check; then
    pass "crash recovery: proxy restarted OK"
else
    fail "crash recovery: proxy failed to restart"
fi

# Verify data survived (PG data should be intact)
AFTER=$(shard_row_count orders)
if [ "$AFTER" -ge "$BEFORE" ]; then
    pass "crash recovery: data intact (before=$BEFORE, after=$AFTER)"
else
    fail "crash recovery: data lost (before=$BEFORE, after=$AFTER)"
fi

# 5b. SIGTERM graceful shutdown
log "5b. SIGTERM graceful shutdown"
SG_PID=$(get_sharegres_pid)
kill -TERM $SG_PID 2>/dev/null
sleep 2
if kill -0 $SG_PID 2>/dev/null; then
    fail "graceful: still alive after SIGTERM"
else
    pass "graceful: clean shutdown"
fi

# Restart for next tests
start_sharegres

# 5c. Rapid restart cycle (10 times)
log "5c. Rapid restart cycle (10 times)"
RESTART_OK=true
for i in $(seq 1 10); do
    SG_PID=$(get_sharegres_pid)
    kill -TERM $SG_PID 2>/dev/null
    sleep 0.5
    start_sharegres
    if ! health_check; then
        fail "restart cycle $i: health check failed"
        RESTART_OK=false
        break
    fi
done
if $RESTART_OK; then
    pass "rapid restart: 10/10 cycles OK"
fi

# 5d. Kill during scatter query
log "5d. Kill during scatter"
(
    for i in $(seq 1 50); do
        psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
            -c "SELECT COUNT(*) FROM orders;" > /dev/null 2>&1
    done
) &
SCATTER_PID=$!
sleep 1

SG_PID=$(get_sharegres_pid)
kill -9 $SG_PID 2>/dev/null
wait $SCATTER_PID 2>/dev/null
sleep 1

start_sharegres
SCATTER_AFTER=$(shard_row_count orders)
if health_check && [ "$SCATTER_AFTER" -gt 0 ]; then
    pass "kill during scatter: recovered, data=$SCATTER_AFTER rows"
else
    fail "kill during scatter: recovery failed"
fi

########################################
# Phase 6: Correctness / Regression
########################################
log ""
log "${BOLD}=== Phase 6: Correctness Tests ===${NC}"

# 6a. Run compat test suite
log "6a. PG compatibility test (100 cases)"
COMPAT=$(cd /data/baotiao/sharegres && ./tests/compat_test.sh $PROXY_PORT $DIRECT_PORT 2>&1 | grep "Compatibility:" | head -1)
if echo "$COMPAT" | grep -q "100%"; then
    pass "compat: $COMPAT"
else
    fail "compat: $COMPAT"
fi

# 6b. Run stress test
log "6b. Stability test (5 rounds)"
STRESS=$(cd /data/baotiao/sharegres && ./tests/stress_test.sh 5 2>&1 | grep -E "ALL.*PASSED|Pass rate" | head -1 | sed 's/\x1b\[[0-9;]*m//g')
if echo "$STRESS" | grep -q "PASSED\|100%"; then
    pass "stress: $STRESS"
else
    fail "stress: $STRESS"
fi

# 6c. SQL edge cases
log "6c. SQL edge cases"
EDGE_OK=true

# Empty result
R=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELECT * FROM orders WHERE user_id = -1;" 2>&1)
[ -z "$R" ] || { fail "edge: empty result not empty"; EDGE_OK=false; }

# Very long query
LONG_PRED=""
for i in $(seq 1 100); do LONG_PRED="${LONG_PRED} OR user_id = $i"; done
R=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELECT COUNT(*) FROM orders WHERE false $LONG_PRED;" 2>/dev/null)
[ -n "$R" ] || { fail "edge: long query failed"; EDGE_OK=false; }

# NULL values
R=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELECT COALESCE(NULL, NULL, 'ok');" 2>&1)
[ "$R" = "ok" ] || { fail "edge: NULL handling: $R"; EDGE_OK=false; }

# Large result through proxy
R=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELECT COUNT(*) FROM generate_series(1, 100000);" 2>&1)
[ "$R" = "100000" ] || { fail "edge: large result: $R"; EDGE_OK=false; }

# Syntax error recovery
psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELEC 1;" > /dev/null 2>&1
R=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
    -c "SELECT 'recovered';" 2>&1)
[ "$R" = "recovered" ] || { fail "edge: error recovery: $R"; EDGE_OK=false; }

if $EDGE_OK; then
    pass "SQL edge cases: all passed"
fi

########################################
# Phase 7: Shard distribution report
########################################
log ""
log "${BOLD}=== Shard Distribution ===${NC}"
for i in $(seq 0 15); do
    c=$(psql -h $PROXY_HOST -p $DIRECT_PORT -U $USER -d $DB -X -A -t \
        -c "SELECT COUNT(*) FROM shard_${i}.orders;" 2>/dev/null)
    printf "  shard_%-2d  %6s orders  " "$i" "$c"
    for b in $(seq 1 $((c / 100))); do printf "█"; done
    echo ""
done

########################################
# Summary
########################################
log ""
TOTAL=$((PASS + FAIL))
echo -e "${BOLD}================================================================${NC}"
if [ $FAIL -eq 0 ]; then
    echo -e "${BOLD}  ${GREEN}ALL $TOTAL TESTS PASSED${NC}"
else
    echo -e "${BOLD}  Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC} / $TOTAL"
fi
echo -e "${BOLD}  Logs: $LOGDIR/${NC}"
echo -e "${BOLD}================================================================${NC}"

exit $FAIL
