#include "config.h"
#include "const.h"
#include "database.h"
#include "decode.h"
#include "dir.h"
#include "download.h"
#include <apr_pools.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return -1;
    }

    apr_status_t rv;
    apr_pool_t *pool;
    rv = apr_initialize();
    if (rv != APR_SUCCESS) {
        return -1;
    }
    apr_pool_create(&pool, NULL);

    config_t *config = apr_palloc(pool, sizeof(config_t));
    config_read(argv[1], config);
    apr_pool_cleanup_register(pool, config, config_free, apr_pool_cleanup_null);

    while (true) {
        apr_pool_t *subpool;
        apr_pool_create(&subpool, pool);

        sqlite3 *db;
        database_open_readonly(config->db, &db);
        config->num_tracks = database_count_tracks(db);

        dir_delete(subpool, config->output);
        dir_create(subpool, config->output);

        file_info_t *file_infos =
            apr_palloc(subpool, config->num_files * sizeof(file_info_t));
        file_infos_t *file_infos_cleaner =
            apr_palloc(subpool, sizeof(file_infos_t));
        file_infos_cleaner->file_infos = file_infos;
        file_infos_cleaner->num_files = config->num_files;
        apr_pool_cleanup_register(subpool, file_infos_cleaner, download_cleanup,
                                  apr_pool_cleanup_null);
        download_init(file_infos, config, db);
        download_files(subpool, file_infos, config);
        assemble_files(file_infos, config);

        sqlite3_close(db);

        for (int i = 0; i < config->num_files; ++i) {
            if (file_infos[i].file_download_status == DOWNLOAD_SUCCEEDED) {
                char *file_path =
                    util_get_file_path(config->output, file_infos[i].filename);

                fprintf(stdout, "%-*s: %s\n", WIDTH + 2, "PLAYING",
                        file_infos[i].filename);
                fprintf(stdout, "  %-*s: %s\n", WIDTH, "path",
                        file_infos[i].album_path);
                fprintf(stdout, "  %-*s: %s\n", WIDTH, "filename",
                        file_infos[i].track_name);

                decode_audio(config, file_path);
                free(file_path);
            }
        }

        apr_pool_destroy(subpool);
    }

    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}