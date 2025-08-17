#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include <apr_thread_mutex.h>

#define LOG_VERSION "0.1.0"

typedef struct {
    va_list ap;
    const char *fmt;
    const char *file;
    struct tm *time;
    void *udata;
    int line;
    int level;
} log_event;

typedef void (*log_fn)(log_event *ev);

enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

static inline const char *get_filename(const char *path) {
    const char *last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

#define log_trace(...) log_log(LOG_TRACE, get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO,  get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN,  get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, get_filename(__FILE__), __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, get_filename(__FILE__), __LINE__, __VA_ARGS__)

const char *log_level_string(int level);
void log_set_level(int level);
void log_set_quiet(bool enable);
int log_add_callback(log_fn fn, void *udata, int level);
int log_add_fp(FILE *fp, int level);

void log_log(int level, const char *file, int line, const char *fmt, ...);

int log_init(apr_pool_t *pool);
void log_shutdown(void);

#endif // LOG_H
