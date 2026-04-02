#ifndef SHAREGRES_EVENT_LOOP_H
#define SHAREGRES_EVENT_LOOP_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include "timer.h"

#define MAX_EVENTS 256

typedef struct EventHandler EventHandler;

/* Callback function type for event handlers */
typedef void (*event_callback_fn)(EventHandler *handler, uint32_t events);

struct EventHandler {
    int                 fd;
    uint32_t            events;     /* registered epoll events */
    void               *data;       /* user data (e.g., ClientSession*) */
    event_callback_fn   callback;
};

typedef struct EventLoop {
    int                 epfd;       /* epoll file descriptor */
    bool                running;
    struct epoll_event  events[MAX_EVENTS];
    TimerQueue          timers;     /* periodic timers */
} EventLoop;

/* Lifecycle */
int     event_loop_init(EventLoop *loop);
void    event_loop_destroy(EventLoop *loop);
void    event_loop_run(EventLoop *loop);
void    event_loop_stop(EventLoop *loop);

/* Event registration */
int     event_loop_add(EventLoop *loop, EventHandler *handler, uint32_t events);
int     event_loop_mod(EventLoop *loop, EventHandler *handler, uint32_t events);
int     event_loop_del(EventLoop *loop, EventHandler *handler);

#endif /* SHAREGRES_EVENT_LOOP_H */
