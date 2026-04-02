#ifndef SHAREGRES_TIMER_H
#define SHAREGRES_TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define MAX_TIMERS 32

typedef void (*timer_callback_fn)(void *data);

typedef struct Timer {
    int             interval_ms;     /* repeat interval in milliseconds */
    timer_callback_fn callback;
    void           *data;
    struct timespec next_fire;       /* next fire time */
    bool            active;
} Timer;

typedef struct TimerQueue {
    Timer   timers[MAX_TIMERS];
    int     count;
} TimerQueue;

void    timer_queue_init(TimerQueue *tq);
int     timer_add(TimerQueue *tq, int interval_ms, timer_callback_fn cb, void *data);
void    timer_remove(TimerQueue *tq, int id);
int     timer_next_timeout_ms(TimerQueue *tq);
void    timer_process_expired(TimerQueue *tq);

#endif /* SHAREGRES_TIMER_H */
