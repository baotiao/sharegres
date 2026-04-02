#!/bin/bash
#
# Sharegres Cluster Manager
#
# Uses a single PostgreSQL instance with multiple databases as shards.
# Each database acts as an independent shard.
#
# Usage:
#   ./scripts/cluster.sh start [num_shards]   - Start cluster (default 10)
#   ./scripts/cluster.sh stop                 - Stop sharegres
#   ./scripts/cluster.sh status               - Show cluster status
#   ./scripts/cluster.sh demo                 - Run a demo workload
#   ./scripts/cluster.sh psql                 - Connect to sharegres proxy
#

set -e

CLUSTER_DIR="/tmp/sharegres_cluster"
SHAREGRES_BIN="/data/baotiao/sharegres/build/sharegres"
SHAREGRES_CONF="$CLUSTER_DIR/sharegres.conf"
SHAREGRES_PORT=15432
PG_HOST=127.0.0.1
PG_PORT=5432
NUM_SHARDS=${2:-10}
USER=$(whoami)

RANGE_MAX=10000000

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log() { echo -e "${CYAN}[cluster]${NC} $*"; }
ok()  { echo -e "  ${GREEN}OK${NC} $*"; }
err() { echo -e "  ${RED}ERROR${NC} $*"; }

do_start() {
    log "Setting up cluster with $NUM_SHARDS shards..."
    mkdir -p "$CLUSTER_DIR"

    # Create databases for each shard
    log "Creating shard databases..."
    for i in $(seq 0 $((NUM_SHARDS - 1))); do
        local dbname="shard_${i}"
        # Check if db exists
        if psql -h $PG_HOST -p $PG_PORT -U $USER -d postgres -X -A -t \
           -c "SELECT 1 FROM pg_database WHERE datname='$dbname'" 2>/dev/null | grep -q 1; then
            ok "$dbname exists"
        else
            psql -h $PG_HOST -p $PG_PORT -U $USER -d postgres -c "CREATE DATABASE $dbname;" 2>&1
            if [ $? -eq 0 ]; then
                ok "$dbname created"
            else
                err "failed to create $dbname"
                return 1
            fi
        fi
    done

    # Create tables on each shard
    log "Creating tables on all shards..."
    for i in $(seq 0 $((NUM_SHARDS - 1))); do
        local dbname="shard_${i}"
        psql -h $PG_HOST -p $PG_PORT -U $USER -d "$dbname" -q << 'TABLESQL' 2>/dev/null
CREATE TABLE IF NOT EXISTS orders (
    id          BIGSERIAL,
    user_id     BIGINT NOT NULL,
    product     TEXT,
    amount      NUMERIC(12,2),
    created_at  TIMESTAMP DEFAULT now(),
    shard_id    INT,
    PRIMARY KEY (user_id, id)
);

CREATE TABLE IF NOT EXISTS users (
    id          BIGINT PRIMARY KEY,
    name        TEXT NOT NULL,
    email       TEXT,
    created_at  TIMESTAMP DEFAULT now(),
    shard_id    INT
);

CREATE TABLE IF NOT EXISTS events (
    id          BIGSERIAL,
    user_id     BIGINT NOT NULL,
    event_type  TEXT,
    payload     JSONB,
    created_at  TIMESTAMP DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_orders_user_id ON orders(user_id);
CREATE INDEX IF NOT EXISTS idx_events_user_id ON events(user_id);
TABLESQL
        ok "tables in $dbname"
    done

    # Generate sharegres config
    log "Generating sharegres config..."
    local range_per_shard=$((RANGE_MAX / NUM_SHARDS))

    cat > "$SHAREGRES_CONF" << SGCONF
[global]
listen_addr = 0.0.0.0
listen_port = $SHAREGRES_PORT
max_clients = 1024
log_level = info

[auth]
method = trust

[pool]
default_pool_size = 10
max_pool_size = 50
idle_timeout = 300

[backend]
conninfo = host=$PG_HOST port=$PG_PORT dbname=postgres

SGCONF

    local shard_names=""
    for i in $(seq 0 $((NUM_SHARDS - 1))); do
        local range_start=$((i * range_per_shard))
        local range_end=$(((i + 1) * range_per_shard))

        cat >> "$SHAREGRES_CONF" << SHARD
[shard:shard_${i}]
conninfo = host=$PG_HOST port=$PG_PORT dbname=shard_${i}
key_range_start = ${range_start}
key_range_end = ${range_end}

SHARD
        if [ $i -gt 0 ]; then shard_names="$shard_names, "; fi
        shard_names="${shard_names}shard_${i}"
    done

    cat >> "$SHAREGRES_CONF" << TABLES
[table:orders]
shard_column = user_id
method = range
shards = $shard_names

[table:users]
shard_column = id
method = range
shards = $shard_names

[table:events]
shard_column = user_id
method = range
shards = $shard_names
TABLES

    ok "Config written to $SHAREGRES_CONF"

    # Start sharegres
    log "Starting sharegres proxy..."
    pkill -f "sharegres.*${SHAREGRES_PORT}" 2>/dev/null || true
    sleep 0.5

    $SHAREGRES_BIN -c "$SHAREGRES_CONF" > "$CLUSTER_DIR/sharegres.log" 2>&1 &
    local sg_pid=$!
    echo $sg_pid > "$CLUSTER_DIR/sharegres.pid"
    sleep 1

    if kill -0 $sg_pid 2>/dev/null; then
        ok "sharegres started (port $SHAREGRES_PORT, pid $sg_pid)"
    else
        err "sharegres failed to start"
        cat "$CLUSTER_DIR/sharegres.log"
        return 1
    fi

    echo ""
    echo -e "${BOLD}============================================${NC}"
    echo -e "${BOLD} Sharegres Cluster Ready${NC}"
    echo -e "${BOLD}============================================${NC}"
    echo ""
    echo -e "  ${BOLD}Proxy:${NC}     psql -h 127.0.0.1 -p $SHAREGRES_PORT"
    echo -e "  ${BOLD}Shards:${NC}    $NUM_SHARDS databases (shard_0 .. shard_$((NUM_SHARDS-1)))"
    echo -e "  ${BOLD}Backend:${NC}   $PG_HOST:$PG_PORT"
    echo -e "  ${BOLD}Range:${NC}     0..${RANGE_MAX} ($((RANGE_MAX / NUM_SHARDS)) per shard)"
    echo -e "  ${BOLD}Tables:${NC}    orders(user_id), users(id), events(user_id)"
    echo ""
}

do_stop() {
    log "Stopping sharegres..."
    if [ -f "$CLUSTER_DIR/sharegres.pid" ]; then
        local sg_pid=$(cat "$CLUSTER_DIR/sharegres.pid")
        if kill -0 $sg_pid 2>/dev/null; then
            kill $sg_pid
            ok "sharegres stopped (pid $sg_pid)"
        fi
        rm -f "$CLUSTER_DIR/sharegres.pid"
    else
        pkill -f "sharegres.*${SHAREGRES_PORT}" 2>/dev/null && ok "sharegres stopped" || true
    fi
}

do_status() {
    echo -e "${BOLD}============================================${NC}"
    echo -e "${BOLD} Sharegres Cluster Status${NC}"
    echo -e "${BOLD}============================================${NC}"

    # Sharegres
    if [ -f "$CLUSTER_DIR/sharegres.pid" ] && kill -0 $(cat "$CLUSTER_DIR/sharegres.pid") 2>/dev/null; then
        echo -e "  Proxy:    ${GREEN}RUNNING${NC} (port $SHAREGRES_PORT, pid $(cat $CLUSTER_DIR/sharegres.pid))"
    else
        echo -e "  Proxy:    ${RED}STOPPED${NC}"
    fi

    # Shard databases
    local total_rows=0
    echo ""
    for i in $(seq 0 19); do
        local dbname="shard_${i}"
        local exists=$(psql -h $PG_HOST -p $PG_PORT -U $USER -d postgres -X -A -t \
                       -c "SELECT 1 FROM pg_database WHERE datname='$dbname'" 2>/dev/null)
        if [ "$exists" = "1" ]; then
            local orders=$(psql -h $PG_HOST -p $PG_PORT -U $USER -d "$dbname" -X -A -t \
                          -c "SELECT COUNT(*) FROM orders;" 2>/dev/null || echo "?")
            local users=$(psql -h $PG_HOST -p $PG_PORT -U $USER -d "$dbname" -X -A -t \
                         -c "SELECT COUNT(*) FROM users;" 2>/dev/null || echo "?")
            local range_per=$((RANGE_MAX / 10))  # approximate
            echo -e "  shard_${i}:  ${GREEN}OK${NC}  orders=$orders  users=$users  range=[$((i * range_per)), $((i * range_per + range_per)))"
            total_rows=$((total_rows + orders + users))
        fi
    done

    echo ""
    echo "  Total rows across shards: $total_rows"
    echo -e "${BOLD}============================================${NC}"
}

do_demo() {
    log "Running demo workload through sharegres proxy..."
    echo ""

    # Insert users spread across shards
    log "Inserting 100 users (spread across 10 shards)..."
    for i in $(seq 1 100); do
        local user_id=$((i * 100000))  # spread across range 0..10000000
        psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres -X -A -t \
            -c "INSERT INTO users(id, name, email, shard_id) VALUES ($user_id, 'user_$i', 'user_$i@example.com', $user_id / 1000000);" 2>/dev/null
    done
    ok "100 users inserted"

    # Insert orders
    log "Inserting 1000 orders (10 per user)..."
    for i in $(seq 1 100); do
        local user_id=$((i * 100000))
        local values=""
        for j in $(seq 1 10); do
            local product="product_$((RANDOM % 50))"
            local amount="$((RANDOM % 10000)).$((RANDOM % 100))"
            if [ -n "$values" ]; then values="$values,"; fi
            values="$values($user_id, '$product', $amount, $user_id / 1000000)"
        done
        psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres -X -A -t \
            -c "INSERT INTO orders(user_id, product, amount, shard_id) VALUES $values;" 2>/dev/null
    done
    ok "1000 orders inserted"

    echo ""
    log "Demo queries:"
    echo ""

    echo -e "${YELLOW}--- Single-shard query (user_id = 500000 -> shard_0) ---${NC}"
    psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres \
        -c "SELECT u.name, COUNT(o.id) as order_count, SUM(o.amount) as total
            FROM users u JOIN orders o ON u.id = o.user_id
            WHERE u.id = 500000
            GROUP BY u.name;" 2>&1
    echo ""

    echo -e "${YELLOW}--- Scatter query: count per shard ---${NC}"
    psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres \
        -c "SELECT shard_id, COUNT(*) as orders, SUM(amount) as total_amount
            FROM orders GROUP BY shard_id ORDER BY shard_id;" 2>&1
    echo ""

    echo -e "${YELLOW}--- Single-shard: top products for user 3000000 (shard_2) ---${NC}"
    psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres \
        -c "SELECT product, COUNT(*), SUM(amount) as total
            FROM orders WHERE user_id = 3000000
            GROUP BY product ORDER BY total DESC LIMIT 5;" 2>&1
    echo ""

    echo -e "${YELLOW}--- Scatter query: total across all shards ---${NC}"
    psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres \
        -c "SELECT COUNT(*) as total_orders, SUM(amount) as total_revenue FROM orders;" 2>&1
    echo ""

    # Show shard distribution
    echo -e "${YELLOW}--- Verify data distribution across shards ---${NC}"
    for i in $(seq 0 $((NUM_SHARDS - 1))); do
        local dbname="shard_${i}"
        local count=$(psql -h $PG_HOST -p $PG_PORT -U $USER -d "$dbname" -X -A -t \
                     -c "SELECT COUNT(*) FROM orders;" 2>/dev/null)
        local users=$(psql -h $PG_HOST -p $PG_PORT -U $USER -d "$dbname" -X -A -t \
                     -c "SELECT COUNT(*) FROM users;" 2>/dev/null)
        printf "  %-10s  orders=%-6s  users=%-4s  " "$dbname" "$count" "$users"
        # Print a bar
        local bar_len=$((count / 2))
        for b in $(seq 1 $bar_len); do printf "█"; done
        echo ""
    done
    echo ""
}

do_psql() {
    exec psql -h 127.0.0.1 -p $SHAREGRES_PORT -U $USER -d postgres
}

case "${1:-}" in
    start)   do_start ;;
    stop)    do_stop ;;
    status)  do_status ;;
    demo)    do_demo ;;
    psql)    do_psql ;;
    *)
        echo "Usage: $0 {start|stop|status|demo|psql} [num_shards]"
        echo ""
        echo "Commands:"
        echo "  start  [N]   Create N shard databases and start sharegres (default 10)"
        echo "  stop         Stop sharegres proxy"
        echo "  status       Show cluster and shard status"
        echo "  demo         Insert demo data and run sample queries"
        echo "  psql         Connect to sharegres proxy"
        exit 1
        ;;
esac
