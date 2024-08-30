#include "config.h"
#include "const.h"
#include "database.h"
#include "decode.h"
#include "dir.h"
#include "download.h"
#include "log.h"
#include <apr_pools.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

void initialize_pool(apr_pool_t **pool) {
    if (apr_initialize() != APR_SUCCESS) {
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

void play_files(file_downloaded_t *file_downloaded, int num_files, char *output,
                char *pipe_name) {
    for (int i = 0; i < num_files; ++i) {
        if (file_downloaded[i].file_download_status == DOWNLOAD_SUCCEEDED) {
            char *file_path =
                util_get_file_path(output, file_downloaded[i].filename);
            log_trace("main: start playing %s", file_path);

            log_trace("PLAYING: %s", file_downloaded[i].filename);
            fprintf(stdout, "%-*s: %s\n", WIDTH + 2, "PLAYING",
                    file_downloaded[i].filename);
            log_trace("path: %s", file_downloaded[i].album_path);
            fprintf(stdout, "  %-*s: %s\n", WIDTH, "path",
                    file_downloaded[i].album_path);
            log_trace("filename: %s", file_downloaded[i].track_name);
            fprintf(stdout, "  %-*s: %s\n", WIDTH, "filename",
                    file_downloaded[i].track_name);

            decode_audio(pipe_name, file_path);
            log_trace("main: finish playing %s", file_path);
            free(file_path);
        }
    }
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

    file_info_t *file_infos =
        apr_palloc(subp1, (*config)->num_files * sizeof(file_info_t));
    file_infos_t *file_infos_cleaner = apr_palloc(subp1, sizeof(file_infos_t));
    file_infos_cleaner->file_infos = file_infos;
    file_infos_cleaner->num_files = (*config)->num_files;
    apr_pool_cleanup_register(subp1, file_infos_cleaner, download_cleanup,
                              apr_pool_cleanup_null);

    download_init(file_infos, *config, db);
    download_files(subp1, file_infos, *config);
    assemble_files(file_infos, *config);

    apr_pool_t *subp2;
    apr_pool_create(&subp2, pool);

    file_downloaded_t *file_downloaded =
        downloaded_files(subp2, file_infos, *config);
    char *output = apr_pstrdup(subp2, (*config)->output);
    char *pipe_name = apr_pstrdup(subp2, (*config)->pipe_name);
    int num_files = (*config)->num_files;

    apr_pool_destroy(subp1);

    play_files(file_downloaded, num_files, output, pipe_name);
    apr_pool_destroy(subp2);
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

    log_trace("start main");
    while (true) {
        log_trace("start while");
        process_files(pool, argv[1], &config);
        log_trace("finish while");
    }
    log_trace("finish main");

    fclose(fp);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}