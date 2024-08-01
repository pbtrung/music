#include "config.h"
#include "database.h"
#include "dir.h"
#include "download.h"
// #include "mpv.h"
#include "ffmpeg.h"

#include <mpv/client.h>
#include <stdbool.h>

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
    // mpv_handle *mpv_ctx = mpv_init(&config);

    while (true) {
        file_info_t *infos =
            (file_info_t *)malloc(config.num_files * sizeof(file_info_t));
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

        download_init(infos, &config, db);
        download_files(infos, &config);
        assemble_files(infos, &config);

        for (int i = 0; i < config.num_files; ++i) {
            printf("\n");
            if (infos[i].download_status == DOWNLOAD_OK) {
                char *file_path =
                    get_file_path(config.output, infos[i].filename);
                const char *cmd[] = {"loadfile", file_path, NULL};

                printf("%-17s: %s\n", "PLAYING", infos[i].filename);
                printf("%-17s: %s\n", "path", infos[i].album_path);
                printf("%-17s: %s\n", "filename", infos[i].track_name);

                // decode_audio(mpv_ctx, cmd);
                decode_audio(file_path, config.pipe_name, infos[i].ext);
                free(file_path);
            }
        }

        download_cleanup(infos, config.num_files);
    }

    // mpv_terminate_destroy(mpv_ctx);
    sqlite3_close(db);
    free_config(&config);

    return 0;
}
