#include "config.h"
#include "log.h"
#include "event/event_loop.h"
#include "session/session.h"
#include "pool/health_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static EventLoop g_loop;
static ShagresCfg *g_cfg;

/* Listener event handler */
typedef struct {
    EventHandler    ev;
    EventLoop      *loop;
    const char     *backend_conninfo;
    ShardMap       *shard_map;
} ListenerCtx;

static void on_signal(int sig)
{
    log_info("received signal %d, shutting down", sig);
    event_loop_stop(&g_loop);
}

static void on_health_check(void *data)
{
    ShardMap *map = (ShardMap *)data;
    health_check_all(map);
}

static int create_listener(const char *addr, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_fatal("socket() failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &saddr.sin_addr);

    if (bind(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        log_fatal("bind(%s:%d) failed: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        log_fatal("listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void on_accept(EventHandler *handler, uint32_t events)
{
    ListenerCtx *ctx = (ListenerCtx *)handler;

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept4(handler->fd,
                                (struct sockaddr *)&client_addr, &addrlen,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            log_error("accept() failed: %s", strerror(errno));
            break;
        }

        /* Enable TCP_NODELAY */
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        log_info("accepted connection from %s:%d fd=%d",
                 ip, ntohs(client_addr.sin_port), client_fd);

        ClientSession *session = session_create(client_fd, ctx->loop,
                                                ctx->backend_conninfo,
                                                ctx->shard_map);
        if (!session) {
            log_error("failed to create session");
            close(client_fd);
        }
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-d basedir] [-c config_file] [-l port] [-b conninfo]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -d DIR     Base directory (loads DIR/sharegres.cnf)\n");
    fprintf(stderr, "  -c FILE    Configuration file path (overrides -d)\n");
    fprintf(stderr, "  -l PORT    Listen port (default: 15432)\n");
    fprintf(stderr, "  -b CONN    Backend connection info\n");
    fprintf(stderr, "  -D         Debug log level\n");
    fprintf(stderr, "  -h         Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -d ./conf\n", prog);
    fprintf(stderr, "  %s -c /etc/sharegres/sharegres.cnf\n", prog);
    fprintf(stderr, "  %s -l 15432 -b \"host=127.0.0.1 port=5432 dbname=postgres\"\n", prog);
}

int main(int argc, char *argv[])
{
    const char *config_file = NULL;
    const char *basedir = NULL;
    const char *cli_backend = NULL;
    int cli_port = 0;
    bool debug = false;

    int opt;
    while ((opt = getopt(argc, argv, "c:d:l:b:Dh")) != -1) {
        switch (opt) {
        case 'c':
            config_file = optarg;
            break;
        case 'd':
            basedir = optarg;
            break;
        case 'l':
            cli_port = atoi(optarg);
            break;
        case 'b':
            cli_backend = optarg;
            break;
        case 'D':
            debug = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Resolve config file path */
    char config_path[1024];
    if (config_file) {
        snprintf(config_path, sizeof(config_path), "%s", config_file);
    } else if (basedir) {
        snprintf(config_path, sizeof(config_path), "%s/sharegres.cnf", basedir);
    } else {
        config_path[0] = '\0';
    }

    /* Load config */
    if (config_path[0]) {
        g_cfg = config_load(config_path);
    } else {
        g_cfg = config_default();
    }
    if (!g_cfg) {
        fprintf(stderr, "failed to load configuration\n");
        return 1;
    }

    /* CLI overrides */
    if (cli_port > 0)
        g_cfg->listen_port = cli_port;
    if (cli_backend) {
        free(g_cfg->backend_conninfo);
        g_cfg->backend_conninfo = strdup(cli_backend);
    }
    if (debug)
        g_cfg->log_level = LOG_DEBUG;

    log_init(g_cfg->log_level);

    log_info("Sharegres starting");
    log_info("  listen: %s:%d", g_cfg->listen_addr, g_cfg->listen_port);
    log_info("  backend: %s", g_cfg->backend_conninfo);

    /* Initialize shard map */
    if (g_cfg->shard_map.shard_count > 0) {
        shard_map_init(&g_cfg->shard_map);
        log_info("  shards: %d, table rules: %d",
                 g_cfg->shard_map.shard_count, g_cfg->shard_map.rule_count);
    }

    /* Signal handling */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize event loop */
    if (event_loop_init(&g_loop) < 0)
        return 1;

    /* Create listener */
    int listener_fd = create_listener(g_cfg->listen_addr, g_cfg->listen_port);
    if (listener_fd < 0)
        return 1;

    ListenerCtx listener_ctx;
    listener_ctx.ev.fd = listener_fd;
    listener_ctx.ev.data = &listener_ctx;
    listener_ctx.ev.callback = on_accept;
    listener_ctx.loop = &g_loop;
    listener_ctx.backend_conninfo = g_cfg->backend_conninfo;
    listener_ctx.shard_map = &g_cfg->shard_map;

    event_loop_add(&g_loop, &listener_ctx.ev, EPOLLIN);

    log_info("Sharegres listening on %s:%d",
             g_cfg->listen_addr, g_cfg->listen_port);

    /* Register health check timer (every 30 seconds) */
    if (g_cfg->shard_map.shard_count > 0) {
        timer_add(&g_loop.timers, 30000, on_health_check, &g_cfg->shard_map);
        log_info("health check timer registered (30s interval)");
    }

    /* Run event loop */
    event_loop_run(&g_loop);

    /* Cleanup */
    close(listener_fd);
    event_loop_destroy(&g_loop);
    config_free(g_cfg);

    log_info("Sharegres stopped");
    return 0;
}
