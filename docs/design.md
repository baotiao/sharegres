# Sharegres 设计文档

### 概述

Sharegres 是一个用 C 语言实现的 PostgreSQL Sharding 中间件. 它位于应用程序和多个 PostgreSQL 实例之间, 对客户端透明地实现数据分片、查询路由和分布式事务管理.

项目设计参考了 [multigres](https://github.com/multigres/multigres) (Go 语言实现的 PG 分片中间件, 灵感来自 Vitess), 但采用 C 语言重写以获得更高性能和更低资源占用, 同时最大程度复用 PostgreSQL 生态的 C 库.

### 设计目标

1. **高性能**: 单进程 epoll reactor 模型, 零拷贝消息转发, 最小化内存分配
2. **低依赖**: 仅依赖 libpq (PG 官方客户端库) 和 libpg_query (PG 解析器提取库)
3. **协议兼容**: 完整实现 PostgreSQL wire protocol, 对客户端完全透明
4. **复用 PG 代码**: 通过 libpg_query 复用 PG 的词法分析器和语法解析器
5. **功能对标 multigres**: 支持查询路由、scatter-gather、分布式事务等核心功能

---

## 一、整体架构

```
                    ┌─────────────────────────────────────┐
                    │             Sharegres                │
                    │                                     │
  psql/app ────►   │  ┌──────────┐    ┌──────────────┐   │   ┌─── PG Shard 0
  (PG wire proto)  │  │ Frontend │    │ Query Router  │   │   │
                   │  │ Protocol │──► │ (libpg_query) │───│───┼─── PG Shard 1
  psql/app ────►   │  │ Handler  │    │               │   │   │    (via libpq)
                   │  └──────────┘    └──────────────┘   │   └─── PG Shard N
                   │        ▲                │            │
                   │        │           ┌────▼─────┐     │
                   │        └───────────│ Executor │     │
                   │                    │ + Pool   │     │
                   │                    └──────────┘     │
                   └─────────────────────────────────────┘
```

### 核心组件

| 组件 | 职责 |
|------|------|
| **Frontend Protocol** | 实现 PG server-side wire protocol, 接受客户端连接 |
| **Session Manager** | 管理客户端会话状态、prepared statements、事务状态 |
| **Query Parser** | 通过 libpg_query 解析 SQL, 生成 AST |
| **Shard Analyzer** | 遍历 AST, 提取目标表名和分片键值 |
| **Router** | 根据分析结果决定路由策略 (单分片/全分片/本地) |
| **Executor** | 执行查询: 单分片直接转发, 多分片 scatter-gather |
| **Transaction Manager** | Deferred BEGIN 模式的分布式事务管理 |
| **Connection Pool** | Per-shard 的 libpq 连接池 |
| **Event Loop** | 基于 epoll 的事件驱动循环 |

### 线程模型

采用**单进程、单线程、epoll reactor** 模型, 与 PgBouncer 类似:

- 所有 I/O 操作都是非阻塞的
- 使用 epoll 同时监听客户端连接和后端 PG 连接
- libpq 使用 non-blocking 模式 (`PQsetnonblocking`)
- 通过 `PQsocket()` 获取后端连接的 fd, 纳入 epoll 管理

这种模型的优点:
- 无锁, 无竞争条件
- 内存占用极低
- 上下文切换开销为零
- 对于 I/O 密集型的代理场景性能优异

---

## 二、PostgreSQL Wire Protocol 实现

### 为什么需要自行实现

libpq 只提供 PostgreSQL **客户端** (client-side) 协议实现, 用于连接 PG 服务器. 但 Sharegres 需要**扮演 PG 服务器**接受客户端连接, 因此必须自行实现 server-side wire protocol.

### 连接建立流程

```
Client                          Sharegres
  │                                │
  │── StartupMessage ─────────────►│  (version=3.0, user, database, ...)
  │                                │
  │◄── AuthenticationOk ───────────│  (v1: trust 模式)
  │◄── ParameterStatus ────────────│  (server_version, client_encoding, ...)
  │◄── ParameterStatus ────────────│  (可多条)
  │◄── BackendKeyData ─────────────│  (process_id, secret_key)
  │◄── ReadyForQuery ──────────────│  (status='I')
  │                                │
  │── Query('SELECT 1') ──────────►│
  │                                │── [路由 + 执行] ──► PG Backend
  │◄── RowDescription ─────────────│
  │◄── DataRow ─────────────────── │
  │◄── CommandComplete ────────────│
  │◄── ReadyForQuery ──────────────│
```

### 消息格式

所有消息 (除 StartupMessage 外) 采用统一格式:

```
┌──────┬──────────┬─────────────┐
│ Type │  Length  │   Payload   │
│ 1B   │  4B (BE)│  Variable   │
└──────┴──────────┴─────────────┘
```

- Type: 1 字节消息类型标识
- Length: 4 字节大端序, 包含自身 4 字节
- Payload: 可变长度的消息体

### 支持的消息类型

**前端消息 (客户端 -> Sharegres):**

| 类型 | 标识 | 说明 |
|------|------|------|
| StartupMessage | (无) | 连接建立, 版本号 + 参数 |
| Query | 'Q' | Simple Query Protocol |
| Parse | 'P' | Extended Query: 准备语句 |
| Bind | 'B' | Extended Query: 绑定参数 |
| Describe | 'D' | Extended Query: 描述语句/portal |
| Execute | 'E' | Extended Query: 执行 portal |
| Sync | 'S' | Extended Query: 同步点 |
| Close | 'C' | 关闭 prepared statement/portal |
| Terminate | 'X' | 断开连接 |
| CopyData | 'd' | COPY 数据传输 |
| CopyDone | 'c' | COPY 完成 |
| CopyFail | 'f' | COPY 失败 |

**后端消息 (Sharegres -> 客户端):**

| 类型 | 标识 | 说明 |
|------|------|------|
| AuthenticationOk | 'R' | 认证成功 |
| ParameterStatus | 'S' | 服务器参数 |
| BackendKeyData | 'K' | 进程 ID + 密钥 |
| ReadyForQuery | 'Z' | 就绪, 含事务状态 |
| RowDescription | 'T' | 结果集列描述 |
| DataRow | 'D' | 结果集数据行 |
| CommandComplete | 'C' | 命令完成 |
| ErrorResponse | 'E' | 错误信息 |
| NoticeResponse | 'N' | 通知信息 |
| ParseComplete | '1' | Parse 完成 |
| BindComplete | '2' | Bind 完成 |
| CloseComplete | '3' | Close 完成 |
| NoData | 'n' | 无数据 |
| CopyInResponse | 'G' | COPY IN 开始 |
| CopyOutResponse | 'H' | COPY OUT 开始 |

---

## 三、SQL 解析与分片分析

### libpg_query 集成

[libpg_query](https://github.com/pganalyze/libpg_query) 从 PostgreSQL 源码中提取了完整的词法分析器和语法解析器, 编译为独立的 C 库. 它能将 SQL 语句解析为与 PG 内部完全一致的 AST (抽象语法树).

```c
#include <pg_query.h>

PgQueryParseResult result = pg_query_parse("SELECT * FROM orders WHERE user_id = 42");
// result.parse_tree -> protobuf 格式的 AST
pg_query_free_parse_result(result);
```

### 分片键提取算法

对解析得到的 AST 进行遍历, 提取路由所需信息:

```
1. 识别语句类型
   ├── SELECT  -> 从 FROM 子句提取表名, WHERE 子句提取分片键
   ├── INSERT  -> 从 INTO 子句提取表名, VALUES 提取分片键值
   ├── UPDATE  -> 从目标表提取表名, WHERE 子句提取分片键
   ├── DELETE  -> 从 FROM 子句提取表名, WHERE 子句提取分片键
   ├── DDL     -> 标记为 SCATTER (广播到所有分片)
   ├── BEGIN/COMMIT/ROLLBACK -> 标记为 TRANSACTION
   └── SET/SHOW/DISCARD     -> 标记为 LOCAL (本地处理)

2. WHERE 子句分析
   └── 查找 "shard_column = <literal_value>" 形式的等值条件
       ├── 找到 -> 提取值, 通过 shard_map 定位目标分片
       └── 未找到 -> 标记为 SCATTER
```

### 路由决策

```c
typedef enum RouteType {
    ROUTE_SINGLE,      /* 路由到单个分片 (有明确分片键) */
    ROUTE_SCATTER,     /* 广播到所有分片 (无分片键或 DDL) */
    ROUTE_LOCAL,       /* 本地处理 (SET/SHOW/DISCARD) */
} RouteType;
```

决策规则:

| 条件 | 路由类型 | 示例 |
|------|---------|------|
| WHERE 中有分片键等值条件 | SINGLE | `SELECT * FROM orders WHERE user_id = 42` |
| INSERT 带有分片键值 | SINGLE | `INSERT INTO orders(user_id, ...) VALUES(42, ...)` |
| 无分片键条件 | SCATTER | `SELECT * FROM orders` |
| DDL 语句 | SCATTER | `CREATE INDEX ...` |
| SET/SHOW/DISCARD | LOCAL | `SET client_encoding = 'UTF8'` |
| BEGIN/COMMIT/ROLLBACK | TRANSACTION | 事务管理器处理 |

---

## 四、分片策略

### Range-based Sharding

将分片键的值域划分为连续的不重叠区间, 每个区间映射到一个分片:

```
Shard 0: [0, 1000000)       -> pg0.example.com
Shard 1: [1000000, 2000000) -> pg1.example.com
Shard 2: [2000000, 3000000) -> pg2.example.com
```

查找算法: 遍历分片列表, 找到 `key_range_start <= value < key_range_end` 的分片.

### Hash-based Sharding (v2 规划)

对分片键值取哈希后取模: `shard_index = hash(value) % shard_count`

### 关键数据结构

```c
/* 分片定义 */
typedef struct Shard {
    char           *name;            /* 分片名, e.g. "shard_0" */
    char           *conninfo;        /* libpq 连接串 */
    uint64_t        key_range_start; /* 范围起始 (inclusive) */
    uint64_t        key_range_end;   /* 范围结束 (exclusive) */
    ConnPool       *pool;            /* 该分片的连接池 */
} Shard;

/* 表分片规则 */
typedef struct TableRule {
    char           *schema_name;     /* schema, 默认 "public" */
    char           *table_name;      /* 表名 */
    char           *shard_column;    /* 分片键列名 */
    ShardMethod     method;          /* SHARD_RANGE / SHARD_HASH */
    bool            broadcast;       /* 是否广播表 (不分片) */
    int             shard_count;
    Shard         **shards;
} TableRule;
```

---

## 五、执行引擎

### 单分片执行

当路由决策为 ROUTE_SINGLE 时:

```
1. 从目标分片的连接池获取 PGconn
2. PQsendQuery(conn, sql)  -- non-blocking
3. epoll 监听 PQsocket(conn) 可读事件
4. PQgetResult(conn) 获取结果
5. 将 PGresult 转换为 PG wire protocol 消息:
   - RowDescription (列描述)
   - DataRow * N (数据行)
   - CommandComplete (命令完成)
6. 发送 ReadyForQuery 给客户端
7. 归还 PGconn 到连接池
```

### Scatter-Gather 执行

当路由决策为 ROUTE_SCATTER 时, 需要将查询广播到所有相关分片:

```
1. 对每个目标分片:
   a. 从连接池获取 PGconn
   b. PQsendQuery(conn, sql)  -- non-blocking
   c. 将 PQsocket(conn) 注册到 epoll

2. epoll 循环:
   a. 等待任意后端连接可读
   b. PQconsumeInput(conn)
   c. while (PQisBusy(conn) == 0):
      - PQgetResult(conn) 获取结果
      - 流式转发 DataRow 给客户端

3. 第一个分片返回结果时, 先发送 RowDescription
4. 后续分片的 DataRow 直接追加转发 (不重复发送 RowDescription)
5. 所有分片完成后:
   - 汇总 rows_affected
   - 发送 CommandComplete + ReadyForQuery
6. 归还所有 PGconn
```

**v1 限制:**
- Scatter 结果不保证排序 (按到达顺序返回)
- 不支持跨分片 ORDER BY / LIMIT / GROUP BY 的下推优化
- 不支持跨分片 JOIN

### 结果合并策略

| 查询类型 | 合并方式 |
|---------|---------|
| SELECT | 流式追加 DataRow |
| INSERT/UPDATE/DELETE | 汇总 rows_affected |
| DDL | 检查所有分片是否成功 |

---

## 六、分布式事务管理

### Deferred BEGIN 模式

参考 multigres 的设计, 采用延迟发送 BEGIN 的策略:

```
时间线:
                Client           Sharegres          Shard 0         Shard 1
                  │                  │                 │               │
  1. BEGIN ──────►│  记录 txn_state  │                 │               │
                  │  = IN_TXN        │                 │               │
                  │  (不发到后端)     │                 │               │
                  │                  │                 │               │
  2. INSERT INTO  │  路由到 shard_0  │                 │               │
     orders ...  ─┤                  ├── BEGIN ───────►│               │
                  │                  ├── INSERT ──────►│               │
                  │                  │◄── OK ──────────│               │
                  │◄── OK ──────────│                  │               │
                  │                  │                 │               │
  3. INSERT INTO  │  路由到 shard_1  │                 │               │
     orders ...  ─┤                  ├── BEGIN ────────┤──────────────►│
                  │                  ├── INSERT ───────┤──────────────►│
                  │                  │◄── OK ──────────┤───────────────│
                  │◄── OK ──────────│                  │               │
                  │                  │                 │               │
  4. COMMIT ─────►│  对所有参与分片  │                 │               │
                  │  发送 COMMIT     ├── COMMIT ──────►│               │
                  │                  ├── COMMIT ───────┤──────────────►│
                  │                  │◄── OK ──────────│               │
                  │                  │◄── OK ──────────┤───────────────│
                  │◄── OK ──────────│                  │               │
```

### 关键设计点

1. **延迟 BEGIN**: 收到 BEGIN 时只记录状态, 不发到任何后端. 当第一条查询路由到某个分片时, 先发 BEGIN 再发查询.

2. **事务连接绑定**: 事务期间, 每个参与的分片都有一个专用的 PGconn (不归还连接池), 保证同一事务的所有查询在同一连接上执行.

3. **COMMIT/ROLLBACK 广播**: 对所有参与事务的分片依次发送 COMMIT 或 ROLLBACK.

4. **连接释放**: 事务结束后, 释放所有绑定的 PGconn 回连接池.

### v1 限制

- **不支持 2PC (两阶段提交)**: 如果 COMMIT 在 shard_0 成功但在 shard_1 失败, 会产生数据不一致. 这是已知限制, 与 multigres v1 行为一致.
- **不支持 SAVEPOINT**: 跨分片的 SAVEPOINT 语义复杂, v1 暂不实现.

### 事务状态机

```
                    BEGIN
    IDLE ──────────────────────► IN_TRANSACTION
     ▲                                │
     │         COMMIT/ROLLBACK        │
     └────────────────────────────────┘
                                      │
                        ERROR         │
                                      ▼
                              IN_FAILED_TRANSACTION
                                      │
                        ROLLBACK      │
                    ┌─────────────────┘
                    ▼
                  IDLE
```

---

## 七、连接池

### 设计

每个 Shard 维护一个独立的连接池:

```c
typedef struct ConnPool {
    Shard          *shard;
    PGconn        **idle_conns;      /* 空闲连接数组 */
    int             idle_count;
    int             max_size;        /* 最大连接数 */
    int             active_count;    /* 当前活跃连接数 */
    int             total_created;   /* 累计创建数 (统计) */
} ConnPool;
```

### 连接生命周期

```
创建 (懒初始化)
  │
  ▼
空闲池 ◄───────────────┐
  │                    │
  │ pool_acquire()     │ pool_release()
  ▼                    │
活跃使用 ──────────────┘
  │
  │ 空闲超时 / 连接异常
  ▼
关闭 (PQfinish)
```

### 核心接口

```c
/* 从池中获取一个连接; 如果无空闲连接且未达上限, 新建 */
PGconn* pool_acquire(ConnPool *pool);

/* 归还连接到池; 如果连接异常则关闭 */
void pool_release(ConnPool *pool, PGconn *conn);

/* 定期健康检查: 关闭异常连接, 回收超时空闲连接 */
void pool_health_check(ConnPool *pool);
```

### 连接使用 libpq non-blocking 模式

```c
PGconn *conn = PQconnectdb(conninfo);
PQsetnonblocking(conn, 1);
int fd = PQsocket(conn);  // 获取 fd, 纳入 epoll
```

---

## 八、事件循环

### 基于 epoll 的 Reactor 模型

```c
typedef struct EventHandler {
    int             fd;
    uint32_t        events;          /* EPOLLIN / EPOLLOUT / EPOLLERR */
    void           *data;            /* ClientSession* 或 BackendConn* */
    void          (*callback)(struct EventHandler *h, uint32_t events);
} EventHandler;

typedef struct EventLoop {
    int             epfd;            /* epoll fd */
    int             listener_fd;     /* 监听 socket */
    bool            running;
    TimerQueue     *timers;          /* 定时器队列 */
} EventLoop;
```

### 事件处理流程

```c
void event_loop_run(EventLoop *loop) {
    struct epoll_event events[MAX_EVENTS];

    while (loop->running) {
        int timeout_ms = timer_next_expiry(loop->timers);
        int n = epoll_wait(loop->epfd, events, MAX_EVENTS, timeout_ms);

        for (int i = 0; i < n; i++) {
            EventHandler *h = events[i].data.ptr;
            h->callback(h, events[i].events);
        }

        timer_process_expired(loop->timers);
    }
}
```

### fd 类型与回调

| fd 类型 | 事件 | 回调行为 |
|---------|------|---------|
| listener_fd | EPOLLIN | accept() 新客户端, 创建 ClientSession |
| client_fd | EPOLLIN | 读取客户端消息, 解析协议, 路由执行 |
| client_fd | EPOLLOUT | 发送缓冲区中的响应数据 |
| backend_fd | EPOLLIN | PQconsumeInput(), 获取结果, 转发给客户端 |
| backend_fd | EPOLLOUT | PQflush(), 发送查询到后端 |

---

## 九、配置系统

### 配置文件格式

采用 INI 风格, 使用 section 区分不同类型的配置:

```ini
[global]
listen_addr = 0.0.0.0
listen_port = 15432
max_clients = 1024
log_level = info

[auth]
method = trust                # trust / md5 / scram

[pool]
default_pool_size = 20
max_pool_size = 100
idle_timeout = 300            # seconds

# 分片定义
[shard:shard_0]
conninfo = host=pg0.example.com port=5432 dbname=mydb
key_range_start = 0
key_range_end = 1000000

[shard:shard_1]
conninfo = host=pg1.example.com port=5432 dbname=mydb
key_range_start = 1000000
key_range_end = 2000000

# 表分片规则
[table:orders]
shard_column = user_id
method = range                # range / hash
shards = shard_0, shard_1

[table:users]
shard_column = id
method = range
shards = shard_0, shard_1

# 广播表 (不分片, 每个分片都有完整副本)
[table:config]
broadcast = true
```

### 配置加载

```c
typedef struct ShagresCfg {
    /* [global] */
    char       *listen_addr;
    int         listen_port;
    int         max_clients;
    LogLevel    log_level;

    /* [auth] */
    AuthMethod  auth_method;

    /* [pool] */
    int         default_pool_size;
    int         max_pool_size;
    int         idle_timeout;

    /* shards & tables */
    Shard      *shards;
    int         shard_count;
    TableRule  *table_rules;
    int         table_rule_count;
} ShagresCfg;

ShagresCfg* config_load(const char *path);
void config_free(ShagresCfg *cfg);
```

---

## 十、项目结构

```
sharegres/
├── CMakeLists.txt                    # 构建系统
├── README.md
├── docs/
│   └── design.md                     # 本文档
├── conf/
│   ├── sharegres.conf.example        # 配置示例
│   └── test.conf                     # 测试用配置
├── src/
│   ├── main.c                        # 入口, 信号处理, 配置加载
│   ├── config.h / config.c           # 配置解析 (INI 格式)
│   ├── log.h / log.c                 # 日志系统
│   ├── protocol/
│   │   ├── fe_protocol.h / .c        # PG server-side wire protocol + COPY 状态机
│   │   └── message.h / .c            # 消息读写工具 + 常用消息构造
│   ├── session/
│   │   └── session.h / .c            # 客户端会话 + 结果转发 + scatter 状态
│   ├── parser/
│   │   ├── query_parser.h / .c       # libpg_query 封装 + AST 分片键提取
│   │   └── json_util.h / .c          # JSON 导航工具 (遍历 parse tree)
│   ├── router/
│   │   ├── router.h / .c             # 路由决策 (SINGLE/SCATTER/LOCAL/DEFAULT)
│   │   └── shard_map.h / .c          # 分片定义 + key range 查找
│   ├── pool/
│   │   ├── conn_pool.h / .c          # Per-shard 连接池
│   │   └── health_check.h / .c       # 定期健康检查
│   ├── executor/
│   │   └── copy_handler.h / .c       # COPY IN/OUT 协议转发
│   └── event/
│       ├── event_loop.h / .c         # epoll 事件循环
│       └── timer.h / .c              # 定时器
├── tests/
│   ├── compat_test.sh                # PG 兼容性测试 (100 cases)
│   ├── stress_test.sh                # 多轮稳定性测试 (25 cases/round)
│   └── bench_10h.sh                  # 长时间 benchmark (pgbench + 自定义负载)
├── scripts/
│   └── cluster.sh                    # 集群管理脚本
└── third_party/
    └── libpg_query/                  # PG 解析器提取库 (vendored)
```

---

## 十一、构建系统

### 依赖

| 依赖 | 版本要求 | 用途 | 引入方式 |
|------|---------|------|---------|
| libpq | PG 14+ | 后端连接 | 系统安装 (postgresql-devel) |
| libpg_query | 16-5.x | SQL 解析 | git submodule |
| POSIX libc | - | epoll, socket | 系统自带 |

### CMake 构建

```cmake
cmake_minimum_required(VERSION 3.10)
project(sharegres C)
set(CMAKE_C_STANDARD 11)

# libpg_query
add_subdirectory(third_party/libpg_query)

# libpq
find_package(PostgreSQL REQUIRED)

add_executable(sharegres
    src/main.c
    src/config.c
    src/log.c
    src/protocol/fe_protocol.c
    src/protocol/fe_auth.c
    src/protocol/message.c
    src/session/session.c
    src/parser/query_parser.c
    src/parser/shard_analyzer.c
    src/router/router.c
    src/router/shard_map.c
    src/executor/executor.c
    src/executor/single_exec.c
    src/executor/scatter_gather.c
    src/executor/txn_manager.c
    src/pool/conn_pool.c
    src/pool/health_check.c
    src/event/event_loop.c
    src/event/timer.c
)

target_include_directories(sharegres PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(sharegres
    pg_query
    ${PostgreSQL_LIBRARIES}
    pthread
)
```

### 编译命令

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

---

## 十二、与 Multigres 的功能对比

| 功能 | Multigres (Go) | Sharegres (C) v1 |
|------|----------------|------------------|
| 查询路由 (单分片) | Yes | Yes |
| Scatter-Gather | Yes | Yes |
| Range Sharding | Yes | Yes |
| Hash Sharding | Yes | v2 规划 |
| 连接池 | Yes (gRPC pooler) | Yes (libpq pool) |
| Simple Query Protocol | Yes | Yes |
| Extended Query Protocol | Yes | v2 规划 |
| COPY IN/OUT 支持 | Yes | Yes |
| AND 条件分片键提取 | Yes | Yes |
| INSERT VALUES 分片键提取 | Yes | Yes |
| 错误转发 (保留 SQLSTATE) | Yes | Yes |
| LISTEN/NOTIFY | Yes | v2 规划 |
| 拓扑发现 (etcd) | Yes | No (配置文件) |
| 自动 Failover | Yes | v2 规划 |
| SCRAM 认证 | Yes | v2 (v1: trust) |
| 2PC | No | No |
| 线程模型 | 多 goroutine | 单线程 epoll (LT) |
| 语言 | Go | C |
| 内存占用 | ~50MB+ | ~4MB |

---

## 十三、实现路线图

### Phase 1: 最小可用代理

目标: psql 能连接 Sharegres 并通过它执行查询 (单后端透传)

- event_loop: epoll reactor 基础框架
- fe_protocol: StartupMessage / AuthenticationOk / ReadyForQuery
- message: 消息读写工具函数
- 单后端: 收到 Query -> libpq 转发到单个 PG -> 结果转发回客户端

**验证:** `psql -h 127.0.0.1 -p 15432 -c "SELECT 1"`

### Phase 2: 配置 + 路由 + 连接池

目标: 根据 SQL 中的分片键路由到正确分片

- config: INI 配置解析
- query_parser + shard_analyzer: libpg_query 集成
- router: 路由决策
- conn_pool: per-shard 连接池

**验证:** 带分片键的查询路由到对应分片, 不同分片键值到不同后端

### Phase 3: Scatter-Gather

目标: 无分片键的查询能返回所有分片的数据

- scatter_gather: async 多分片执行
- 结果合并与流式转发

**验证:** `SELECT * FROM orders` 返回所有分片数据

### Phase 4: 事务 + Extended Query Protocol

目标: 跨分片事务和 prepared statements

- txn_manager: deferred BEGIN, 分布式 COMMIT/ROLLBACK
- prepared_stmt: Parse/Bind/Execute
- session 增强: 事务状态跟踪

**验证:** BEGIN -> INSERT 到多个分片 -> COMMIT, 数据一致

### Phase 5: 完善

- COPY 支持
- 查询改写 (query rewriting)
- 健康检查
- 优雅退出 (graceful shutdown)
- 日志系统完善

---

## 十四、测试策略与结果

### 兼容性测试 (compat_test.sh)

对比通过代理和直连 PG 的输出, 100 个测试用例覆盖 18 个类别:

| 类别 | 测试数 | 覆盖内容 |
|------|--------|---------|
| 数据类型 | 12 | int/bigint/float/numeric/text/bool/date/bytea/uuid/inet/json/jsonb/NULL |
| NULL 处理 | 5 | IS NULL, COALESCE, NULLIF, 聚合中的 NULL |
| 表达式与函数 | 10 | 算术/字符串/数学/日期/CASE/CAST/布尔/LIKE/正则 |
| 聚合函数 | 7 | COUNT/SUM/AVG/MIN/MAX/GROUP BY/HAVING/DISTINCT/array_agg/string_agg |
| 子查询 | 6 | scalar/IN/EXISTS/NOT EXISTS/correlated/ANY/ALL |
| JOIN | 6 | INNER/LEFT/RIGHT/FULL OUTER/CROSS/self |
| 窗口函数 | 5 | ROW_NUMBER/RANK/DENSE_RANK/SUM OVER/LAG/LEAD/PARTITION BY |
| CTE | 3 | simple/multiple/recursive |
| JSON | 6 | ->>/#>>/contains/@>/keys/array_elements/build_object |
| 数组 | 5 | access/ANY/length/unnest/append |
| INSERT | 5 | RETURNING/multi-row/ON CONFLICT/DO NOTHING/from SELECT |
| UPDATE/DELETE | 4 | RETURNING/subquery |
| COPY | 3 | STDOUT/CSV/HEADER |
| 事务 | 4 | implicit/BEGIN-COMMIT/BEGIN-ROLLBACK/verify |
| DDL | 5 | CREATE/INSERT/SELECT/ALTER/DROP |
| 会话命令 | 7 | generate_series/LIMIT-OFFSET/UNION/INTERSECT/EXCEPT |
| 大结果集 | 2 | 1000 行/10KB 文本 |
| 错误处理 | 5 | 语法错误/表不存在/类型不匹配/除零/唯一约束 |

**结果: 100% (100/100)**

```bash
./tests/compat_test.sh 15432 5432
```

### 稳定性测试 (stress_test.sh)

每轮 25 个测试, 覆盖分片路由正确性、数据隔离、CRUD、SQL 特性:

```bash
./tests/stress_test.sh 10
# ALL 250 TESTS PASSED (10 rounds)
```

### 性能测试 (bench_10h.sh)

轮换 pgbench 和自定义分片负载, 持续运行:

```bash
./tests/bench_10h.sh 10  # 10 小时
```

10 分片集群 benchmark 结果:

| 工作负载 | TPS | 失败数 |
|---------|-----|-------|
| pgbench SELECT (4 clients, 30min) | 42.7 | 0 |
| pgbench TPC-B (4 clients, 30min) | 13.3 | 0 |
| pgbench SELECT (8 clients, 30min) | 58.5 | 0 |
| 自定义分片负载 (INSERT+SELECT+UPDATE x 10 shards, 30min) | ~326 ops/s | 0 |

注意: pgbench 需要使用 `-M simple` 参数, 因为 Extended Query Protocol 尚未支持.

---

## 十五、实现关键笔记

### epoll 模式选择: LT vs ET

最初使用 EPOLLET (edge-triggered) 模式, 在调试中发现关键问题:

1. **后端结果丢失**: `PQconnectdb` (同步) + `PQsendQuery` 后, 后端响应可能在 `event_loop_add` 之前就到达. ET 模式下不会再次通知, 导致 backend_readable 永远不被调用.
2. **客户端消息积压**: 客户端在同一个 TCP 包中发送 Query + Terminate, ET 模式下第二次 recv 不触发, Terminate 留在 recv_buf 中永远不被处理.

**解决方案**: 改用 LT (level-triggered) 模式. 对代理场景, LT 模式更简单可靠, PgBouncer 也使用 LT.

### CommandComplete 消息

libpq 的 `PGRES_TUPLES_OK` 状态在结果转发时需要手动发送 CommandComplete 消息. 初始实现遗漏了这一点, 导致 psql 收不到完整响应 (有 RowDescription + DataRow 但缺少 CommandComplete, psql 不显示任何内容).

对于 INSERT/UPDATE/DELETE RETURNING, `PQresultStatus` 返回 `PGRES_TUPLES_OK` (而非 `PGRES_COMMAND_OK`), 此时 `PQcmdStatus` 返回正确的 command tag (如 "INSERT 0 1").

### ErrorResponse 字段转发

后端错误需要通过 `PQresultErrorField` 分别提取 severity (PG_DIAG_SEVERITY), SQLSTATE (PG_DIAG_SQLSTATE), message (PG_DIAG_MESSAGE_PRIMARY) 等字段, 而不是使用 `PQresultErrorMessage` (它返回格式化后的完整字符串, 直接放入 ErrorResponse 的 'M' 字段会导致重复前缀).

### WHERE AND 条件的分片键提取

对于 `WHERE user_id = 42 AND status = 'active'` 形式的查询, PG 解析器生成 BoolExpr(AND) 包含两个 A_Expr 子节点. 需要递归遍历 BoolExpr 的 args 数组, 逐个检查是否有匹配分片列的等值条件.

初始实现中 `json_find_all_values` 在整个 JSON 子树中搜索 "sval" key, 会匹配到 operator name 中的 "sval" (如 `"sval": "="`), 导致提取到错误的列名. 修复为使用 `json_find_key_recursive` 精确定位 ColumnRef -> fields -> String -> sval 路径.
