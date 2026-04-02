cmake_minimum_required(VERSION 3.10)
project(sharegres C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -Wno-unused-parameter -g -O2)
add_compile_definitions(_GNU_SOURCE)

# Find libpq (PostgreSQL client library)
find_package(PostgreSQL REQUIRED)

# libpg_query (pre-built static library)
set(LIBPG_QUERY_DIR ${CMAKE_SOURCE_DIR}/third_party/libpg_query)

# Source files
set(SHAREGRES_SOURCES
    src/main.c
    src/config.c
    src/log.c
    src/protocol/fe_protocol.c
    src/protocol/message.c
    src/session/session.c
    src/event/event_loop.c
    src/event/timer.c
    src/pool/health_check.c
    src/executor/copy_handler.c
    src/parser/json_util.c
    src/parser/query_parser.c
    src/router/router.c
    src/router/shard_map.c
    src/pool/conn_pool.c
)

add_executable(sharegres ${SHAREGRES_SOURCES})

target_include_directories(sharegres PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PostgreSQL_INCLUDE_DIRS}
    ${LIBPG_QUERY_DIR}
)

target_link_libraries(sharegres
    ${LIBPG_QUERY_DIR}/libpg_query.a
    ${PostgreSQL_LIBRARIES}
    pthread
    m
)

# Install target
install(TARGETS sharegres RUNTIME DESTINATION bin)
