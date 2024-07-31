#include "config.h"
#include "database.h"
#include "dir.h"
#include "downloader.h"
#include "random.h"
#include "track.h"
#include <stdbool.h>
#include <string.h>
#include <uv.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return -1;
    }

    config_t config;
    read_config(argv[1], &config);

    sqlite3 *db;
    if (open_database_readonly(config.db, &db) != SQLITE_OK) {
        return -1;
    }

    config.num_tracks = count_tracks(db);
    uv_loop_t *loop = uv_default_loop();

    while (true) {
        file_downloader_t *infos =
            malloc(config.num_files * sizeof(file_downloader_t));
        if (!infos) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }

        int rc = delete_directory(config.output);
        if (rc != 0) {
            fprintf(stderr, "delete_directory\n");
            exit(-1);
        }
        create_directory(config.output);

        initialize_downloads(infos, config.num_files, &config, db);
        perform_downloads(infos, config.num_files, loop);

        uv_run(loop, UV_RUN_DEFAULT);

        // char *json = track_extract_metadata(infos, config.num_files);
        // track_decode(infos);

        cleanup_downloads(infos, config.num_files);
        free(infos);
        // free(json);
    }

    uv_loop_close(loop);
    sqlite3_close(db);
    free_config(&config);

    return 0;
}
