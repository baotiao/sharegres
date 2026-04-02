#include "log.h"
#include <stdlib.h>
#include <string.h>

LogLevel g_log_level = LOG_INFO;

static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

void log_init(LogLevel level)
{
    g_log_level = level;
}

void log_msg(LogLevel level, const char *file, int line, const char *fmt, ...)
{
    if (level < g_log_level)
        return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "%s [%s] %s:%d: ", timebuf, level_names[level], file, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");

    if (level == LOG_FATAL) {
        exit(1);
    }
}
