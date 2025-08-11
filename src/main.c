#include <stdio.h>
#include <stdlib.h>

#include <apr_pools.h>

#include "config.h"
#include "database.h"
#include "dir.h"
#include "download.h"
#include "log.h"
#include "queue.h"

typedef struct {
    file_queue_t *queue;
    apr_pool_t *pool;
    const char *config_file;
    config_t *config;
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
    config_free(*config);
    free(*config);
    *config = NULL;

    return fp;
}

static void *APR_THREAD_FUNC downloader_thread(apr_thread_t *thd, void *data) {
    (void)thd;
    downloader_args_t *args = (downloader_args_t *)data;
    file_queue_t *q = args->queue;
    apr_pool_t *pool = args->pool;
    config_t *config = args->config;
    const char *config_file = args->config_file;

    apr_pool_t *subpool;
    apr_pool_create(&subpool, pool);

    config = apr_palloc(subpool, sizeof(config_t));
    config_read(config_file, config);
    apr_pool_cleanup_register(subpool, config, config_free,
                              apr_pool_cleanup_null);

    sqlite3 *db;
    database_open_readonly(config->db, &db);
    apr_pool_cleanup_register(subpool, db, database_close,
                              apr_pool_cleanup_null);

    config->num_tracks = database_count_tracks(db);
    dir_delete(subpool, config->output);
    dir_create(subpool, config->output);

    download_assemble_files(subpool, db, config);

    apr_pool_destroy(subpool);
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

    file_queue_t queue;
    queue_init(&queue, pool);
    apr_thread_t *dl_thread = NULL;
    apr_threadattr_t *dl_attr = NULL;
    downloader_args_t dl_args = {.queue = &queue,
                                 .pool = pool,
                                 .config_file = argv[1],
                                 .config = config};
    apr_threadattr_create(&dl_attr, pool);
    apr_thread_create(&dl_thread, dl_attr, downloader_thread, &dl_args, pool);

    fclose(fp);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}