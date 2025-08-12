#include <stdio.h>
#include <stdlib.h>

#include <apr_pools.h>
#include <apr_queue.h>
#include <apr_thread_proc.h>

#include "config.h"
#include "database.h"
#include "dir.h"
#include "download.h"
#include "log.h"
#include "utils.h"

typedef struct {
    char *filename;
    char *pipe_name;
    char *file_path;
} file_task_t;

typedef struct {
    apr_queue_t *queue;
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

static void process_file(apr_pool_t *subpool, config_t *cfg, apr_queue_t *q) {
    sqlite3 *db;
    file_info_t *info = prepare_file_info(subpool, cfg, &db);
    download_assemble_file(subpool, db, cfg, info);

    if (info->file_download_status != DOWNLOAD_SUCCEEDED)
        return;

    file_task_t *task = malloc(sizeof(file_task_t));
    if (!task) {
        log_trace("process_file: Failed malloc");
        exit(-1);
    }
    // Make heap copies so they survive pool destruction
    task->filename = strdup(info->filename);
    task->pipe_name = strdup(cfg->pipe_name);
    if (!task->filename || !task->pipe_name) {
        log_trace("process_file: Failed strdup");
        exit(-1);
    }
    task->file_path = util_get_file_path(cfg->output, info->filename);

    apr_status_t rv = apr_queue_push(q, task);
    if (rv != APR_SUCCESS) {
        log_trace("process_file: Failed to push %s", task->file_path);
        if (remove(task->file_path) != 0) {
            log_trace("process_file: Failed to delete %s", task->file_path);
        }
        exit(-1);
    }
}

static void run_downloader(apr_pool_t *pool, const char *cfg_file,
                           config_t **cfg, apr_queue_t *q) {
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
    // signal producer done
    apr_queue_term(args->queue);
    log_trace("downloader_thread: end");
    return NULL;
}

static void consume_files(apr_queue_t *queue) {
    for (int i = 0; i < 3; ++i) {
        file_task_t *task = NULL;
        apr_status_t rv = apr_queue_pop(queue, (void **)&task);
        if (rv == APR_EOF) {
            // producer finished
            break;
        }
        if (rv != APR_SUCCESS || !task->file_path) {
            // skip invalid entries
            continue;
        }
        fprintf(stdout, "main: queue_pop: filename=%s\n", task->filename);
        log_trace("main: queue_pop: filename=%s", task->filename);
        apr_sleep(apr_time_from_sec(60));
        if (remove(task->file_path) != 0) {
            log_trace("main: Failed to delete file %s", task->file_path);
            exit(-1);
        }

        // Free heap memory
        free(task->filename);
        free(task->pipe_name);
        free(task->file_path);
        free(task);
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

    apr_queue_t *queue;
    apr_status_t rv = apr_queue_create(&queue, cfg->num_files, pool);
    if (rv != APR_SUCCESS) {
        fprintf(stderr, "Failed to create APR queue\n");
        exit(-1);
    }

    config_free(cfg);
    free(cfg);
    cfg = NULL;

    apr_thread_t *dl_thread;
    apr_threadattr_t *dl_attr;
    apr_threadattr_create(&dl_attr, pool);
    downloader_args_t dl_args = {
        .queue = queue, .pool = pool, .config_file = argv[1], .config = &cfg};
    apr_thread_create(&dl_thread, dl_attr, downloader_thread, &dl_args, pool);

    consume_files(queue);

    apr_thread_join(&rv, dl_thread);

    fclose(fp);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}
