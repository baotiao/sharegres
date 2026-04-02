# Sharegres

A PostgreSQL sharding middleware written in C. Sits between your application and multiple PostgreSQL instances, transparently routing queries to the correct shard.

Inspired by [multigres](https://github.com/multigres/multigres) (Go-based PG sharding middleware, itself inspired by Vitess), rewritten in C for maximum performance and minimal dependencies.

## Architecture

```
                    ┌─────────────────────────────────┐
                    │           Sharegres              │
  psql/app ───►    │  ┌──────────┐  ┌──────────────┐  │  ┌── PG Shard 0
  (PG wire proto)  │  │ Frontend │  │ Query Router  │  │  │
                   │  │ Protocol │─►│ (libpg_query) │──│──┼── PG Shard 1
  psql/app ───►    │  │ Handler  │  │               │  │  │   (via libpq)
                   │  └──────────┘  └──────────────┘  │  └── PG Shard N
                   │        ▲              │           │
                   │        └──────────────┘           │
                   └─────────────────────────────────┘
```

- **Single process, epoll reactor** - no threads, no locks, similar to PgBouncer
- **Frontend**: custom PG server-side wire protocol implementation (accepts client connections)
- **Backend**: libpq in non-blocking mode (connects to PG instances)
- **Parser**: libpg_query (extracted PG parser, generates AST from SQL)
- **Router**: AST analysis extracts table name + shard key from WHERE/VALUES clauses

## Features

| Feature | Status |
|---------|--------|
| PG Wire Protocol (Simple Query) | Done |
| SQL Parsing via libpg_query | Done |
| Range-based Sharding | Done |
| Single-shard Query Routing | Done |
| Scatter-Gather (multi-shard) | Done |
| Per-shard Connection Pooling | Done |
| COPY IN / COPY OUT | Done |
| AND WHERE clause shard key extraction | Done |
| INSERT shard key extraction from VALUES | Done |
| Error forwarding (preserves SQLSTATE) | Done |
| Health check timer | Done |
| Graceful shutdown (SIGTERM) | Done |
| INI config file | Done |
| Extended Query Protocol (Parse/Bind/Execute) | Planned |
| Hash-based Sharding | Planned |
| 2PC Distributed Transactions | Not planned |

## Quick Start

### Build

```bash
# Dependencies: libpq (postgresql-devel), cmake, gcc
# libpg_query is included as a vendored dependency

# Build libpg_query first
cd third_party/libpg_query && make -j$(nproc) && cd ../..

# Build sharegres
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### Run (single backend proxy)

```bash
# Proxy all queries to a single PG instance
./build/sharegres -l 15432 -b "host=127.0.0.1 port=5432 dbname=postgres"

# Connect through the proxy
psql -h 127.0.0.1 -p 15432 -d postgres
```

### Run (sharded cluster)

Create a config file:

```ini
[global]
listen_addr = 0.0.0.0
listen_port = 15432
log_level = info

[pool]
default_pool_size = 10

[backend]
conninfo = host=127.0.0.1 port=5432 dbname=postgres

[shard:shard_0]
conninfo = host=pg0.example.com port=5432 dbname=mydb
key_range_start = 0
key_range_end = 5000000

[shard:shard_1]
conninfo = host=pg1.example.com port=5432 dbname=mydb
key_range_start = 5000000
key_range_end = 10000000

[table:orders]
shard_column = user_id
method = range
shards = shard_0, shard_1

[table:users]
shard_column = id
method = range
shards = shard_0, shard_1
```

```bash
./build/sharegres -c sharegres.conf
```

### Cluster management script

```bash
# Start a 10-shard cluster (uses PG schemas for isolation)
./scripts/cluster.sh start 10

# Check status
./scripts/cluster.sh status

# Connect
./scripts/cluster.sh psql

# Stop
./scripts/cluster.sh stop
```

## How Routing Works

Sharegres parses each SQL statement using libpg_query and extracts:
1. **Target table** from FROM / INTO / UPDATE clauses
2. **Shard key value** from WHERE equality conditions or INSERT VALUES

```
SELECT * FROM orders WHERE user_id = 42;
  -> table: orders, shard_key: user_id = 42
  -> route: SINGLE (shard_0, because 42 is in [0, 5000000))

SELECT * FROM orders;
  -> table: orders, no shard key
  -> route: SCATTER (query all shards, merge results)

INSERT INTO orders(user_id, amount) VALUES(42, 100);
  -> table: orders, shard_key: user_id = 42 (from VALUES)
  -> route: SINGLE (shard_0)

SET client_encoding = 'UTF8';
  -> route: LOCAL (handled by proxy)
```

Compound WHERE clauses with AND are supported:
```
UPDATE orders SET amount = 0 WHERE user_id = 42 AND status = 'pending';
  -> extracts user_id = 42 from the AND condition
  -> route: SINGLE (shard_0)
```

## Testing

### Compatibility test (100 cases)

Compares proxy output vs direct PG for 18 categories: data types, NULL handling, expressions, aggregates, subqueries, JOINs, window functions, CTEs, JSON, arrays, DML, COPY, transactions, DDL, session commands, large results, error handling.

```bash
./tests/compat_test.sh 15432 5432
# Compatibility: 100% (100/100)
```

### Stability test (10 rounds, 250 cases)

Tests shard routing correctness, data isolation, CRUD operations, SQL features across multiple rounds.

```bash
./tests/stress_test.sh 10
# ALL 250 TESTS PASSED (10 rounds)
```

### Long-running benchmark (pgbench + custom workloads)

```bash
# Rotates pgbench (select-only, TPC-B) and custom sharded workloads
./tests/bench_10h.sh 10  # 10 hours
```

Benchmark results (2h sample, 10-shard cluster):

| Workload | TPS | Failed |
|----------|-----|--------|
| pgbench SELECT (4 clients, 30min) | 42.7 | 0 |
| pgbench TPC-B (4 clients, 30min) | 13.3 | 0 |
| pgbench SELECT (8 clients, 30min) | 58.5 | 0 |
| Custom shard ops (30min) | ~326 ops/s (587K total) | 0 |

## Project Structure

```
src/
├── main.c              # Entry point, signal handling, config loading
├── config.c            # INI config parser
├── log.c               # Logging
├── protocol/
│   ├── fe_protocol.c   # PG server-side wire protocol
│   └── message.c       # Message read/write utilities
├── session/
│   └── session.c       # Client session state machine, result forwarding
├── parser/
│   ├── query_parser.c  # libpg_query wrapper, AST shard key extraction
│   └── json_util.c     # JSON navigator for parse tree
├── router/
│   ├── router.c        # Routing decisions (SINGLE/SCATTER/LOCAL/DEFAULT)
│   └── shard_map.c     # Shard definitions, key range lookup
├── pool/
│   ├── conn_pool.c     # Per-shard connection pool (libpq)
│   └── health_check.c  # Periodic health checks
├── executor/
│   └── copy_handler.c  # COPY IN/OUT protocol forwarding
└── event/
    ├── event_loop.c    # epoll event loop
    └── timer.c         # Periodic timer (health checks)
```

## Dependencies

| Dependency | Purpose | How |
|-----------|---------|-----|
| libpq | Backend PG connections | System install (postgresql-devel) |
| libpg_query | SQL parsing (PG parser) | Vendored in third_party/ |
| POSIX libc | epoll, sockets | System |

No other external dependencies.

## Design Document

See [docs/design.md](docs/design.md) for the full technical design covering wire protocol, sharding strategies, execution engine, connection pooling, and configuration.

## Known Limitations

- **Extended Query Protocol**: Parse/Bind/Execute not yet supported (pgbench init, some ORMs). Use Simple Query protocol (`-M simple` for pgbench).
- **No 2PC**: Distributed transactions use sequential COMMIT. If one shard fails, others may already be committed.
- **Scatter results unordered**: Multi-shard SELECT returns rows in arrival order, not sorted.
- **No cross-shard JOINs**: JOINs only work within a single shard.

## License

MIT
