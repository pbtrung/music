#include "queue.h"

void queue_init(file_queue_t *q, apr_pool_t *pool) {
    q->head = q->tail = q->size = 0;
    q->pool = pool;
    q->producer_done = 0;

    // Allocate array of char* (rows)
    q->paths = apr_pcalloc(pool, q->config->num_files * sizeof(char *));

    // Allocate each string buffer
    for (int i = 0; i < q->config->num_files; i++) {
        q->paths[i] = apr_pcalloc(pool, q->config->max_pathlen * sizeof(char));
    }

    apr_thread_mutex_create(&q->mutex, APR_THREAD_MUTEX_UNNESTED, pool);
    apr_thread_cond_create(&q->not_empty, pool);
    apr_thread_cond_create(&q->not_full, pool);
}

void queue_destroy(file_queue_t *q) {
    if (q->mutex)
        apr_thread_mutex_destroy(q->mutex);
    if (q->not_empty)
        apr_thread_cond_destroy(q->not_empty);
    if (q->not_full)
        apr_thread_cond_destroy(q->not_full);
}

int queue_push(file_queue_t *q, const char *path) {
    apr_thread_mutex_lock(q->mutex);
    while (q->size == q->config->num_files) {
        apr_thread_cond_wait(q->not_full, q->mutex);
    }
    strncpy(q->paths[q->tail], path, q->config->max_pathlen - 1);
    q->paths[q->tail][q->config->max_pathlen - 1] = '\0';
    q->tail = (q->tail + 1) % q->config->num_files;
    q->size++;
    apr_thread_cond_signal(q->not_empty);
    apr_thread_mutex_unlock(q->mutex);
    return 1;
}

int queue_pop(file_queue_t *q, char *out_path) {
    apr_thread_mutex_lock(q->mutex);
    while (q->size == 0 && !q->producer_done) {
        apr_thread_cond_wait(q->not_empty, q->mutex);
    }
    if (q->size == 0) {
        apr_thread_mutex_unlock(q->mutex);
        return 0;
    }
    strncpy(out_path, q->paths[q->head], q->config->max_pathlen);
    q->head = (q->head + 1) % q->config->num_files;
    q->size--;
    apr_thread_cond_signal(q->not_full);
    apr_thread_mutex_unlock(q->mutex);
    return 1;
}

void queue_mark_done(file_queue_t *q) {
    apr_thread_mutex_lock(q->mutex);
    q->producer_done = 1;
    apr_thread_cond_broadcast(q->not_empty);
    apr_thread_mutex_unlock(q->mutex);
}