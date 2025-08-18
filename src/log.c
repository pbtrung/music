#include <apr_pools.h>
#include <apr_time.h>

#include "log.h"

#define MAX_CALLBACKS 32

typedef struct {
    log_fn fn;
    void *udata;
    int level;
} callback;

static struct {
    int level;
    bool quiet;
    callback callbacks[MAX_CALLBACKS];
    apr_thread_mutex_t *mutex;
    apr_pool_t *pool;
} L;

static const char *level_strings[] = {"TRACE", "DEBUG", "INFO",
                                      "WARN",  "ERROR", "FATAL"};

static void stdout_callback(log_event *ev) {
    char buf[16];
    buf[strftime(buf, sizeof(buf), "%H:%M:%S", ev->time)] = '\0';
    fprintf(ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level],
            ev->file, ev->line);
    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");
    fflush(ev->udata);
}

static void file_callback(log_event *ev) {
    char buf[64];
    buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
    fprintf(ev->udata, "%s %-5s %s:%d: ", buf, level_strings[ev->level],
            ev->file, ev->line);
    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");
    fflush(ev->udata);
}

static void lock(void) {
    if (L.mutex)
        apr_thread_mutex_lock(L.mutex);
}

static void unlock(void) {
    if (L.mutex)
        apr_thread_mutex_unlock(L.mutex);
}

const char *log_level_string(int level) {
    return level_strings[level];
}

void log_set_level(int level) {
    L.level = level;
}

void log_set_quiet(bool enable) {
    L.quiet = enable;
}

int log_add_callback(log_fn fn, void *udata, int level) {
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (!L.callbacks[i].fn) {
            L.callbacks[i] = (callback){fn, udata, level};
            return 0;
        }
    }
    return -1;
}

int log_add_fp(FILE *fp, int level) {
    return log_add_callback(file_callback, fp, level);
}

static void init_event(log_event *ev, void *udata) {
    if (!ev->time) {
        time_t t = time(NULL);
        ev->time = localtime(&t);
    }
    ev->udata = udata;
}

void log_log(int level, const char *file, int line, const char *fmt, ...) {
    log_event ev = {
        .fmt = fmt,
        .file = file,
        .line = line,
        .level = level,
    };

    lock();

    if (!L.quiet && level >= L.level) {
        init_event(&ev, stderr);
        va_start(ev.ap, fmt);
        stdout_callback(&ev);
        va_end(ev.ap);
    }

    for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
        callback *cb = &L.callbacks[i];
        if (level >= cb->level) {
            init_event(&ev, cb->udata);
            va_start(ev.ap, fmt);
            cb->fn(&ev);
            va_end(ev.ap);
        }
    }

    unlock();
}

int log_init(apr_pool_t *pool) {
    L.pool = pool;
    L.level = LOG_TRACE;
    L.quiet = false;
    for (int i = 0; i < MAX_CALLBACKS; i++)
        L.callbacks[i].fn = NULL;
    if (apr_thread_mutex_create(&L.mutex, APR_THREAD_MUTEX_DEFAULT, pool) !=
        APR_SUCCESS) {
        return -1;
    }
    return 0;
}

void log_shutdown(void) {
    if (L.mutex) {
        apr_thread_mutex_destroy(L.mutex);
        L.mutex = NULL;
    }
}
