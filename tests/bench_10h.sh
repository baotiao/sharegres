#!/bin/bash
#
# Sharegres 10-Hour Benchmark Suite
#
# Rotates pgbench + custom sharded workloads for 10 hours.
#
# Usage: ./tests/bench_10h.sh [hours]
#

PROXY_HOST=127.0.0.1
PROXY_PORT=15432
DIRECT_PORT=5432
USER=$(whoami)
DB=postgres
LOGDIR="/tmp/sharegres_cluster/bench_logs"
TOTAL_HOURS=${1:-10}
CYCLE_MINUTES=30

PGBENCH=$(which pgbench)
mkdir -p "$LOGDIR"

STARTTIME=$(date +%s)
ENDTIME=$((STARTTIME + TOTAL_HOURS * 3600))

log() {
    local ts=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$ts] $*" | tee -a "$LOGDIR/bench_main.log"
}

health_check() {
    local result
    result=$(psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t -c "SELECT 1;" 2>&1)
    if [ "$result" = "1" ]; then
        return 0
    else
        log "HEALTH CHECK FAILED: $result"
        return 1
    fi
}

run_pgbench_select() {
    local duration=$((CYCLE_MINUTES * 60))
    local logfile="$LOGDIR/pgbench_select_$(date +%Y%m%d_%H%M%S).log"
    log ">>> pgbench SELECT-only (${CYCLE_MINUTES}min, 4 clients)"

    $PGBENCH -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
        -M simple -c 4 -j 2 -T $duration -S -P 300 \
        > "$logfile" 2>&1
    local rc=$?
    local tps=$(grep "^tps = " "$logfile" | tail -1)
    local failed=$(grep "number of failed" "$logfile" | head -1)
    log "    Result: rc=$rc $tps | $failed"
}

run_pgbench_tpcb() {
    local duration=$((CYCLE_MINUTES * 60))
    local logfile="$LOGDIR/pgbench_tpcb_$(date +%Y%m%d_%H%M%S).log"
    log ">>> pgbench TPC-B (${CYCLE_MINUTES}min, 4 clients)"

    $PGBENCH -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
        -M simple -c 4 -j 2 -T $duration -P 300 \
        > "$logfile" 2>&1
    local rc=$?
    local tps=$(grep "^tps = " "$logfile" | tail -1)
    local failed=$(grep "number of failed" "$logfile" | head -1)
    log "    Result: rc=$rc $tps | $failed"
}

run_pgbench_ro_8c() {
    local duration=$((CYCLE_MINUTES * 60))
    local logfile="$LOGDIR/pgbench_ro8c_$(date +%Y%m%d_%H%M%S).log"
    log ">>> pgbench SELECT-only (${CYCLE_MINUTES}min, 8 clients)"

    $PGBENCH -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB \
        -M simple -c 8 -j 4 -T $duration -S -P 300 \
        > "$logfile" 2>&1
    local rc=$?
    local tps=$(grep "^tps = " "$logfile" | tail -1)
    local failed=$(grep "number of failed" "$logfile" | head -1)
    log "    Result: rc=$rc $tps | $failed"
}

run_custom_shard() {
    local duration=$((CYCLE_MINUTES * 60))
    local logfile="$LOGDIR/custom_shard_$(date +%Y%m%d_%H%M%S).log"
    log ">>> Custom sharded workload (${CYCLE_MINUTES}min)"

    local end_ts=$(($(date +%s) + duration))
    local total_ops=0
    local total_errors=0
    local batch=0

    while [ $(date +%s) -lt $end_ts ]; do
        batch=$((batch + 1))
        local batch_ops=0
        local batch_errors=0

        for i in $(seq 1 50); do
            local shard=$((RANDOM % 10))
            local uid=$((shard * 1000000 + RANDOM % 1000000))

            # INSERT
            psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
                -c "INSERT INTO orders(user_id, product, amount) VALUES ($uid, 'b$((RANDOM % 100))', $((RANDOM % 9999)).$((RANDOM % 99)));" \
                > /dev/null 2>&1 && batch_ops=$((batch_ops + 1)) || batch_errors=$((batch_errors + 1))

            # SELECT single shard
            psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
                -c "SELECT COUNT(*) FROM orders WHERE user_id = $uid;" \
                > /dev/null 2>&1 && batch_ops=$((batch_ops + 1)) || batch_errors=$((batch_errors + 1))

            # UPDATE
            psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
                -c "UPDATE orders SET amount = $((RANDOM % 9999)) WHERE user_id = $uid AND product = 'b$((RANDOM % 100))';" \
                > /dev/null 2>&1 && batch_ops=$((batch_ops + 1)) || batch_errors=$((batch_errors + 1))
        done

        # Scatter query
        psql -h $PROXY_HOST -p $PROXY_PORT -U $USER -d $DB -X -A -t \
            -c "SELECT COUNT(*) FROM orders;" > /dev/null 2>&1 && batch_ops=$((batch_ops + 1)) || batch_errors=$((batch_errors + 1))

        total_ops=$((total_ops + batch_ops))
        total_errors=$((total_errors + batch_errors))

        echo "$(date '+%H:%M:%S') batch=$batch ops=$batch_ops err=$batch_errors total_ops=$total_ops total_err=$total_errors" >> "$logfile"

        # Log every 10 batches
        if [ $((batch % 10)) -eq 0 ]; then
            log "    shard batch=$batch total_ops=$total_ops errors=$total_errors"
        fi
    done

    log "    Custom shard done: total_ops=$total_ops errors=$total_errors"
}

run_stability_check() {
    local logfile="$LOGDIR/stability_$(date +%Y%m%d_%H%M%S).log"
    log ">>> Stability test (compat + stress)"

    cd /data/baotiao/sharegres

    # Run compat test
    log "    Running compat test..."
    local compat_result=$(./tests/compat_test.sh 15432 5432 2>&1 | grep "Compatibility:" | head -1)
    log "    Compat: $compat_result"

    # Run stress test (3 rounds)
    log "    Running 3-round stress test..."
    local stress_result=$(./tests/stress_test.sh 3 2>&1 | grep "Pass rate\|ALL.*PASSED" | head -1)
    log "    Stress: $stress_result"

    echo "$compat_result" >> "$logfile"
    echo "$stress_result" >> "$logfile"
}

# ─── Main ───

log "============================================"
log " Sharegres ${TOTAL_HOURS}-Hour Benchmark Suite"
log " Proxy: $PROXY_HOST:$PROXY_PORT"
log " Workloads: pgbench(select,tpcb,8c), custom_shard, stability"
log " Cycle: ${CYCLE_MINUTES} min per workload"
log " Logs: $LOGDIR/"
log "============================================"

health_check || { log "FATAL: proxy not responding"; exit 1; }

CYCLE=0
while [ $(date +%s) -lt $ENDTIME ]; do
    CYCLE=$((CYCLE + 1))
    ELAPSED_H=$(( ($(date +%s) - STARTTIME) / 3600 ))
    ELAPSED_M=$(( (($(date +%s) - STARTTIME) % 3600) / 60 ))
    REMAINING=$(( (ENDTIME - $(date +%s)) / 60 ))
    log ""
    log "=== Cycle $CYCLE | Elapsed: ${ELAPSED_H}h${ELAPSED_M}m | Remaining: ${REMAINING}min ==="

    health_check || { log "FATAL: proxy down!"; break; }

    case $((CYCLE % 5)) in
        1) run_pgbench_select ;;
        2) run_pgbench_tpcb ;;
        3) run_custom_shard ;;
        4) run_pgbench_ro_8c ;;
        0) run_stability_check ;;
    esac
done

log ""
log "============================================"
log " Benchmark Complete"
log " Total cycles: $CYCLE"
log " Duration: ${TOTAL_HOURS} hours"
log "============================================"

health_check && log "Final: HEALTHY" || log "Final: UNHEALTHY"

# Print summary
log ""
log "--- Summary ---"
grep "Result:\|done:" "$LOGDIR/bench_main.log" | tail -20
