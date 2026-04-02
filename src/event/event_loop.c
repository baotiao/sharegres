#include "event_loop.h"
#include "../log.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>

int event_loop_init(EventLoop *loop)
{
    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) {
        log_error("epoll_create1 failed: %s", strerror(errno));
        return -1;
    }
    loop->running = false;
    timer_queue_init(&loop->timers);
    return 0;
}

void event_loop_destroy(EventLoop *loop)
{
    if (loop->epfd >= 0) {
        close(loop->epfd);
        loop->epfd = -1;
    }
}

void event_loop_run(EventLoop *loop)
{
    loop->running = true;
    log_info("event loop started");

    while (loop->running) {
        int timeout_ms = timer_next_timeout_ms(&loop->timers);
        int n = epoll_wait(loop->epfd, loop->events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            EventHandler *h = loop->events[i].data.ptr;
            if (h && h->callback) {
                h->callback(h, loop->events[i].events);
            }
        }

        timer_process_expired(&loop->timers);
    }

    log_info("event loop stopped");
}

void event_loop_stop(EventLoop *loop)
{
    loop->running = false;
}

int event_loop_add(EventLoop *loop, EventHandler *handler, uint32_t events)
{
    handler->events = events;

    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = handler;

    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, handler->fd, &ev) < 0) {
        log_error("epoll_ctl ADD fd=%d failed: %s", handler->fd, strerror(errno));
        return -1;
    }
    return 0;
}

int event_loop_mod(EventLoop *loop, EventHandler *handler, uint32_t events)
{
    handler->events = events;

    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = handler;

    if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, handler->fd, &ev) < 0) {
        log_error("epoll_ctl MOD fd=%d failed: %s", handler->fd, strerror(errno));
        return -1;
    }
    return 0;
}

int event_loop_del(EventLoop *loop, EventHandler *handler)
{
    if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, handler->fd, NULL) < 0) {
        log_error("epoll_ctl DEL fd=%d failed: %s", handler->fd, strerror(errno));
        return -1;
    }
    return 0;
}
