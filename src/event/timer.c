#include "timer.h"
#include <string.h>

static void timespec_add_ms(struct timespec *ts, int ms)
{
    ts->tv_nsec += (long)ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int64_t timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000 +
           (a->tv_nsec - b->tv_nsec) / 1000000;
}

void timer_queue_init(TimerQueue *tq)
{
    memset(tq, 0, sizeof(TimerQueue));
}

int timer_add(TimerQueue *tq, int interval_ms, timer_callback_fn cb, void *data)
{
    if (tq->count >= MAX_TIMERS)
        return -1;

    int id = tq->count;
    Timer *t = &tq->timers[id];
    t->interval_ms = interval_ms;
    t->callback = cb;
    t->data = data;
    t->active = true;

    clock_gettime(CLOCK_MONOTONIC, &t->next_fire);
    timespec_add_ms(&t->next_fire, interval_ms);

    tq->count++;
    return id;
}

void timer_remove(TimerQueue *tq, int id)
{
    if (id >= 0 && id < tq->count)
        tq->timers[id].active = false;
}

int timer_next_timeout_ms(TimerQueue *tq)
{
    if (tq->count == 0)
        return 1000; /* default 1s */

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int min_ms = 1000;
    for (int i = 0; i < tq->count; i++) {
        if (!tq->timers[i].active)
            continue;

        int64_t diff = timespec_diff_ms(&tq->timers[i].next_fire, &now);
        if (diff <= 0)
            return 0;
        if (diff < min_ms)
            min_ms = (int)diff;
    }

    return min_ms;
}

void timer_process_expired(TimerQueue *tq)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (int i = 0; i < tq->count; i++) {
        Timer *t = &tq->timers[i];
        if (!t->active)
            continue;

        int64_t diff = timespec_diff_ms(&t->next_fire, &now);
        if (diff <= 0) {
            t->callback(t->data);

            /* Reschedule */
            clock_gettime(CLOCK_MONOTONIC, &t->next_fire);
            timespec_add_ms(&t->next_fire, t->interval_ms);
        }
    }
}
