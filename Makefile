# Sharegres top-level Makefile
#
# Usage:
#   make          - build everything, output to ./bin/
#   make clean    - remove build artifacts
#   make test     - run compatibility + stress tests
#   make bench    - run 1-hour benchmark
#

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -Wno-unused-parameter -g -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS  ?=

# Directories
SRCDIR   := src
BINDIR   := bin
OBJDIR   := build/obj
DEPDIR   := third_party/libpg_query

# Find PostgreSQL
PG_CONFIG ?= pg_config
PG_INCLUDEDIR := $(shell $(PG_CONFIG) --includedir 2>/dev/null || echo /usr/include/postgresql)
PG_LIBDIR     := $(shell $(PG_CONFIG) --libdir 2>/dev/null || echo /usr/lib64)

CFLAGS  += -I$(SRCDIR) -I$(DEPDIR) -I$(PG_INCLUDEDIR)
LDFLAGS += -L$(PG_LIBDIR) -lpq -lpthread -lm

# libpg_query static library
LIBPG_QUERY := $(DEPDIR)/libpg_query.a

# Source files
SRCS := $(SRCDIR)/main.c \
        $(SRCDIR)/config.c \
        $(SRCDIR)/log.c \
        $(SRCDIR)/protocol/fe_protocol.c \
        $(SRCDIR)/protocol/message.c \
        $(SRCDIR)/session/session.c \
        $(SRCDIR)/event/event_loop.c \
        $(SRCDIR)/event/timer.c \
        $(SRCDIR)/parser/json_util.c \
        $(SRCDIR)/parser/query_parser.c \
        $(SRCDIR)/router/router.c \
        $(SRCDIR)/router/shard_map.c \
        $(SRCDIR)/pool/conn_pool.c \
        $(SRCDIR)/pool/health_check.c \
        $(SRCDIR)/executor/copy_handler.c

OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
TARGET := $(BINDIR)/sharegres

.PHONY: all clean test bench deps

all: $(TARGET)
	@echo ""
	@echo "Build complete: $(TARGET)"
	@echo "  Run:  ./bin/sharegres -h"

$(TARGET): deps $(OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(OBJS) $(LIBPG_QUERY) $(LDFLAGS) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Build libpg_query (only the library, skip tests)
deps: $(LIBPG_QUERY)

$(LIBPG_QUERY):
	@echo "Building libpg_query..."
	$(MAKE) -C $(DEPDIR) build
	@echo "libpg_query built."

clean:
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "Clean complete."

distclean: clean
	$(MAKE) -C $(DEPDIR) clean 2>/dev/null || true

test: $(TARGET)
	@echo "=== Compatibility Test ==="
	./tests/compat_test.sh 15432 5432
	@echo ""
	@echo "=== Stress Test (3 rounds) ==="
	./tests/stress_test.sh 3

bench: $(TARGET)
	./tests/bench_10h.sh 1
