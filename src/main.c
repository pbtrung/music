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

static void initialize_pool(apr_pool_t **pool) {
    if (apr_initialize() != APR_SUCCESS) {
        fprintf(stderr, "Failed to init APR\n");
        exit(-1);
    }
    apr_pool_create(pool, NULL);
}

static config_t *load_config(const char *config_file) {
    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg) {
        fprintf(stderr, "Memory allocation failed");
        exit(-1);
    }
    config_read(config_file, cfg);
    return cfg;
}

static FILE *open_log_file(config_t *cfg) {
    FILE *fp = fopen(cfg->log, "w");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", cfg->log);
        exit(-1);
    }
    log_add_fp(fp, LOG_TRACE);
    log_set_quiet(true);
    return fp;
}

static file_info_t *prepare_file_info(apr_pool_t *subpool, config_t *cfg,
                                      sqlite3 **db) {
    database_open_readonly(cfg->db, db);
    apr_pool_cleanup_register(subpool, *db, database_close,
                              apr_pool_cleanup_null);
    cfg->num_tracks = database_count_tracks(*db);
    file_info_t *info = apr_palloc(subpool, sizeof(file_info_t));
    int *rand_idx = util_random_ints(1, cfg->min_value, cfg->num_tracks);
    file_info_init(info, *rand_idx, *db, cfg);
    free(rand_idx);
    apr_pool_cleanup_register(subpool, info, file_info_free,
                              apr_pool_cleanup_null);
    return info;
}

static void process_file(apr_pool_t *subpool, config_t *cfg, file_queue_t *q) {
    sqlite3 *db;
    file_info_t *info = prepare_file_info(subpool, cfg, &db);
    download_assemble_file(subpool, db, cfg, info);

    if (info->file_download_status != DOWNLOAD_SUCCEEDED)
        return;
    
    char *file_path = util_get_file_path(cfg->output, info->filename);
    if (!queue_push(q, file_path)) {
        log_trace("process_file: Failed to push %s", file_path);
        if (remove(file_path) != 0) {
            log_trace("process_file: Failed to delete %s", file_path);
        }
        exit(-1);
    }
    free(file_path);
}

static void run_downloader(apr_pool_t *pool, const char *cfg_file,
                           config_t **cfg, file_queue_t *q) {
    for (int i = 0; i < 3; ++i) {
        log_trace("downloader_thread: start loop");
        apr_pool_t *subpool;
        apr_pool_create(&subpool, pool);
        *cfg = apr_palloc(subpool, sizeof(config_t));
        config_read(cfg_file, *cfg);
        apr_pool_cleanup_register(subpool, *cfg, config_free,
                                  apr_pool_cleanup_null);
        process_file(subpool, *cfg, q);
        apr_pool_destroy(subpool);
        log_trace("downloader_thread: end loop");
    }
}

static void *APR_THREAD_FUNC downloader_thread(apr_thread_t *thd, void *data) {
    log_trace("downloader_thread: start");
    downloader_args_t *args = data;
    run_downloader(args->pool, args->config_file, args->config, args->queue);
    queue_mark_done(args->queue);
    log_trace("downloader_thread: end");
    return NULL;
}

static void consume_files(file_queue_t *queue, char *file_path) {
    for (int i = 0; i < 3; ++i) {
        if (!queue_pop(queue, file_path))
            break;
        fprintf(stdout, "main: queue_pop: file_path: %s\n", file_path);
        apr_sleep(apr_time_from_sec(60));
        if (remove(file_path) != 0) {
            log_trace("main: Failed to delete file %s", file_path);
            exit(-1);
        }
    }
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return -1;
    }

    apr_pool_t *pool;
    initialize_pool(&pool);
    config_t *cfg = load_config(argv[1]);
    FILE *fp = open_log_file(cfg);

    dir_delete(pool, cfg->output);
    dir_create(pool, cfg->output);

    file_queue_t queue = {.num_files = cfg->num_files,
                          .max_pathlen = cfg->max_pathlen};
    queue_init(&queue, pool);
    char *file_path = apr_palloc(pool, cfg->max_pathlen);
    config_free(cfg);
    free(cfg);
    cfg = NULL;

    apr_thread_t *dl_thread;
    apr_threadattr_t *dl_attr;
    apr_threadattr_create(&dl_attr, pool);
    downloader_args_t dl_args = {
        .queue = &queue, .pool = pool, .config_file = argv[1], .config = &cfg};
    apr_thread_create(&dl_thread, dl_attr, downloader_thread, &dl_args, pool);

    consume_files(&queue, file_path);

    apr_thread_mutex_lock(queue.mutex);
    apr_thread_cond_broadcast(queue.not_empty);
    apr_thread_cond_broadcast(queue.not_full);
    apr_thread_mutex_unlock(queue.mutex);

    apr_status_t rv;
    apr_thread_join(&rv, dl_thread);

    fclose(fp);
    queue_destroy(&queue);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}
