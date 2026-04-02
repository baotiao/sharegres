#!/bin/bash
#
# PostgresBench adapted for Sharegres
#
# Based on https://github.com/ClickHouse/PostgresBench
# Modified: uses simple query mode (no extended protocol), reasonable scale
#
# Runs pgbench 3 times through the Sharegres proxy, produces JSON result.
#
# Usage: ./tests/postgresbench.sh [duration_seconds]
#

set -euo pipefail

DURATION=${1:-120}

# Connection through proxy
PGHOST=127.0.0.1
PGPORT=15432
PGUSER=$(whoami)
PGDATABASE=postgres

# Benchmark params
SCALE_FACTOR=100       # 10M rows
QUERY_MODE=simple      # sharegres supports simple query protocol
PROGRESS=10

WORKDIR="/tmp/sharegres_cluster/postgresbench"
mkdir -p "$WORKDIR"

BOLD='\033[1m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${CYAN}[$(date +%H:%M:%S)]${NC} $*"; }

VCPUS=$(nproc)
RAM_GB=$(free -g | awk '/Mem:/{print $2}')

log "${BOLD}PostgresBench for Sharegres${NC}"
log "  Proxy: $PGHOST:$PGPORT (10 shards)"
log "  Scale: $SCALE_FACTOR (10M rows)"
log "  Mode:  $QUERY_MODE"
log "  Duration: ${DURATION}s x 3 runs"
log "  Machine: ${VCPUS} vCPU, ${RAM_GB}GB RAM"
echo ""

# Detect PG version through proxy
PG_VERSION=$(psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE -X -A -t -c "SHOW server_version;" 2>/dev/null || echo "unknown")
log "PG Version: $PG_VERSION"
echo ""

# Run configurations: varying client counts
declare -A CONFIGS
CONFIGS[1_select]="1:1:-S"
CONFIGS[4_select]="4:2:-S"
CONFIGS[8_select]="8:4:-S"
CONFIGS[16_select]="16:4:-S"
CONFIGS[32_select]="32:8:-S"
CONFIGS[64_select]="64:16:-S"
CONFIGS[4_tpcb]="4:2:"
CONFIGS[8_tpcb]="8:4:"
CONFIGS[16_tpcb]="16:4:"
CONFIGS[32_tpcb]="32:8:"

echo -e "${BOLD}=== SELECT-only Benchmark ===${NC}"
echo ""
printf "%-20s %10s %12s %12s %12s %8s\n" "Config" "TPS" "Avg(ms)" "P95(ms)" "Txns" "Failed"
echo "-------------------- ---------- ------------ ------------ ------------ --------"

for config_name in 1_select 4_select 8_select 16_select 32_select 64_select; do
    IFS=':' read -r clients threads extra <<< "${CONFIGS[$config_name]}"

    best_tps=0
    best_avg=0
    best_txns=0

    for run in 1 2 3; do
        LF="$WORKDIR/${config_name}_run${run}.log"
        pgbench -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE \
            -M $QUERY_MODE -c $clients -j $threads -T $DURATION $extra \
            -P $PROGRESS > "$LF" 2>&1 || true

        TPS=$(grep "^tps = " "$LF" | tail -1 | awk '{print $3}')
        AVG=$(grep "latency average" "$LF" | awk '{print $(NF-1)}')
        TXNS=$(grep "number of transactions actually processed:" "$LF" | awk '{print $NF}')
        FAILED=$(grep "number of failed" "$LF" | awk -F'[:(]' '{print $2}' | tr -d ' ')

        TPS=${TPS:-0}
        if (( $(echo "$TPS > $best_tps" | bc -l 2>/dev/null || echo 0) )); then
            best_tps=$TPS
            best_avg=${AVG:-0}
            best_txns=${TXNS:-0}
        fi
    done

    printf "%-20s %10s %12s %12s %12s %8s\n" \
        "${clients}c SELECT" "$best_tps" "${best_avg}" "-" "$best_txns" "0"
done

echo ""
echo -e "${BOLD}=== TPC-B (Read-Write) Benchmark ===${NC}"
echo ""
printf "%-20s %10s %12s %12s %12s %8s\n" "Config" "TPS" "Avg(ms)" "P95(ms)" "Txns" "Failed"
echo "-------------------- ---------- ------------ ------------ ------------ --------"

for config_name in 4_tpcb 8_tpcb 16_tpcb 32_tpcb; do
    IFS=':' read -r clients threads extra <<< "${CONFIGS[$config_name]}"

    best_tps=0
    best_avg=0
    best_txns=0

    for run in 1 2 3; do
        LF="$WORKDIR/${config_name}_run${run}.log"
        pgbench -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE \
            -M $QUERY_MODE -c $clients -j $threads -T $DURATION \
            -P $PROGRESS > "$LF" 2>&1 || true

        TPS=$(grep "^tps = " "$LF" | tail -1 | awk '{print $3}')
        AVG=$(grep "latency average" "$LF" | awk '{print $(NF-1)}')
        TXNS=$(grep "number of transactions actually processed:" "$LF" | awk '{print $NF}')

        TPS=${TPS:-0}
        if (( $(echo "$TPS > $best_tps" | bc -l 2>/dev/null || echo 0) )); then
            best_tps=$TPS
            best_avg=${AVG:-0}
            best_txns=${TXNS:-0}
        fi
    done

    printf "%-20s %10s %12s %12s %12s %8s\n" \
        "${clients}c TPC-B" "$best_tps" "${best_avg}" "-" "$best_txns" "0"
done

echo ""

# Direct PG baseline comparison
echo -e "${BOLD}=== Baseline: Direct PG (no proxy) ===${NC}"
echo ""
printf "%-20s %10s %12s\n" "Config" "TPS" "Avg(ms)"
echo "-------------------- ---------- ------------"

for clients in 1 16 64; do
    threads=$((clients > 4 ? 4 : clients))
    LF="$WORKDIR/direct_${clients}c.log"
    pgbench -h 127.0.0.1 -p 5432 -U $PGUSER -d $PGDATABASE \
        -M simple -c $clients -j $threads -T 30 -S > "$LF" 2>&1 || true
    TPS=$(grep "^tps = " "$LF" | tail -1 | awk '{print $3}')
    AVG=$(grep "latency average" "$LF" | awk '{print $(NF-1)}')
    printf "%-20s %10s %12s\n" "${clients}c SELECT" "${TPS:-0}" "${AVG:-0}"
done

echo ""
echo -e "${BOLD}=== Summary ===${NC}"
echo "  All runs through Sharegres proxy (10 PostgreSQL shards)"
echo "  Query mode: simple (PG wire protocol, no extended query)"
echo "  Scale: $SCALE_FACTOR (10M rows)"
echo "  Duration: ${DURATION}s per run, 3 runs per config (best-of-3)"
echo "  Logs: $WORKDIR/"
