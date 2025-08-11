#include <stdio.h>
#include <stdlib.h>

#include <apr_pools.h>

#include "config.h"
#include "database.h"
#include "dir.h"
#include "download.h"
#include "log.h"

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

void process_files(apr_pool_t *pool, const char *config_file,
                   config_t **config) {
    apr_pool_t *subp1;
    apr_pool_create(&subp1, pool);

    *config = apr_palloc(subp1, sizeof(config_t));
    config_read(config_file, *config);
    apr_pool_cleanup_register(subp1, *config, config_free,
                              apr_pool_cleanup_null);

    sqlite3 *db;
    database_open_readonly((*config)->db, &db);
    apr_pool_cleanup_register(subp1, db, database_close, apr_pool_cleanup_null);

    (*config)->num_tracks = database_count_tracks(db);
    dir_delete(subp1, (*config)->output);
    dir_create(subp1, (*config)->output);

    download_assemble_files(subp1, db, *config);

    apr_pool_destroy(subp1);
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

    process_files(pool, argv[1], &config);

    fclose(fp);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}