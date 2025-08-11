#include <stdio.h>
#include <stdlib.h>

#include <apr_pools.h>

#include "config.h"
#include "database.h"
#include "dir.h"
#include "download.h"
#include "log.h"
#include "queue.h"
#include "utils.h"

typedef struct {
    file_queue_t *queue;
    apr_pool_t *pool;
    const char *config_file;
    config_t **config;
} downloader_args_t;

void initialize_pool(apr_pool_t **pool) {
    if (apr_initialize() != APR_SUCCESS) {
        fprintf(stderr, "Failed to initialize APR\n");
        exit(-1);
    }
    apr_pool_create(pool, NULL);
}

FILE *setup_logging(const char *config_file, config_t **config) {
    *config = malloc(sizeof(config_t));
    if (!(*config)) {
        fprintf(stderr, "Memory allocation failed");
        exit(-1);
    }
    config_read(config_file, *config);
    FILE *fp = fopen((*config)->log, "w");
    if (!fp) {
        fprintf(stderr, "Failed to open file %s\n", (*config)->log);
        exit(-1);
    }
    log_add_fp(fp, LOG_TRACE);
    log_set_quiet(true);

    return fp;
}

static void *APR_THREAD_FUNC downloader_thread(apr_thread_t *thd, void *data) {
    (void)thd;
    downloader_args_t *args = (downloader_args_t *)data;
    file_queue_t *q = args->queue;
    apr_pool_t *pool = args->pool;
    config_t **config = args->config;
    const char *config_file = args->config_file;

    for (int i = 0; i < 3; ++i) {
        apr_pool_t *subpool;
        apr_pool_create(&subpool, pool);

        *config = apr_palloc(subpool, sizeof(config_t));
        config_read(config_file, *config);
        apr_pool_cleanup_register(subpool, *config, config_free,
                                  apr_pool_cleanup_null);

        sqlite3 *db;
        database_open_readonly((*config)->db, &db);
        apr_pool_cleanup_register(subpool, db, database_close,
                                  apr_pool_cleanup_null);
        (*config)->num_tracks = database_count_tracks(db);

        file_info_t *info = apr_palloc(subpool, sizeof(file_info_t));
        int *random_index =
            util_random_ints(1, (*config)->min_value, (*config)->num_tracks);
        file_info_init(info, *random_index, db, *config);
        free(random_index);
        apr_pool_cleanup_register(subpool, info, file_info_free,
                                  apr_pool_cleanup_null);
        download_assemble_file(subpool, db, *config, info);

        if (info->file_download_status == DOWNLOAD_SUCCEEDED) {
            char *file_path =
                util_get_file_path((*config)->output, info->filename);
            if (!queue_push(q, file_path)) {
                log_trace("downloader_thread: Failed to push: %s", file_path);
                if (remove(file_path) != 0) {
                    log_trace("append_cid_output: Failed to delete file %s",
                              file_path);
                    exit(-1);
                }
            }
            free(file_path);
        }

        apr_pool_destroy(subpool);
    }
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return -1;
    }

    apr_pool_t *pool;
    initialize_pool(&pool);

    config_t *config = NULL;
    FILE *fp = setup_logging(argv[1], &config);

    dir_delete(pool, config->output);
    dir_create(pool, config->output);

    file_queue_t queue = {.num_files = config->num_files,
                          .max_pathlen = config->max_pathlen};
    queue_init(&queue, pool);
    config_free(config);
    free(config);
    config = NULL;

    apr_thread_t *dl_thread = NULL;
    apr_threadattr_t *dl_attr = NULL;
    downloader_args_t dl_args = {.queue = &queue,
                                 .pool = pool,
                                 .config_file = argv[1],
                                 .config = &config};
    apr_threadattr_create(&dl_attr, pool);
    apr_thread_create(&dl_thread, dl_attr, downloader_thread, &dl_args, pool);

    /* request stop and wake any waiting threads */
    apr_thread_mutex_lock(queue.mutex);
    apr_thread_cond_broadcast(queue.not_empty);
    apr_thread_cond_broadcast(queue.not_full);
    apr_thread_mutex_unlock(queue.mutex);
    apr_status_t rv;
    apr_thread_join(&rv, dl_thread);

    fclose(fp);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}