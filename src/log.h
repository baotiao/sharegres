#ifndef SHAREGRES_LOG_H
#define SHAREGRES_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
} LogLevel;

extern LogLevel g_log_level;

void log_init(LogLevel level);
void log_msg(LogLevel level, const char *file, int line, const char *fmt, ...);

#define log_debug(...) log_msg(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_msg(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif /* SHAREGRES_LOG_H */
