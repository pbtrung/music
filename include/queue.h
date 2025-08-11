#ifndef QUEUE_H
#define QUEUE_H

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_thread_cond.h>
#include <apr_thread_mutex.h>
#include <apr_thread_proc.h>

#include "config.h"

typedef struct {
    char **paths;
    int head;
    int tail;
    int size;
    apr_thread_mutex_t *mutex;
    apr_thread_cond_t *not_empty;
    apr_thread_cond_t *not_full;
    apr_pool_t *pool;
    int producer_done;
    int num_files;
    int max_pathlen;
} file_queue_t;

void queue_init(file_queue_t *q, apr_pool_t *pool);
void queue_destroy(file_queue_t *q);
int queue_push(file_queue_t *q, const char *path);
int queue_pop(file_queue_t *q, char *out_path);
void queue_mark_done(file_queue_t *q);

#endif // QUEUE_H