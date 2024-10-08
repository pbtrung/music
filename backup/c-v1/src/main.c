#include "config.h"
#include "database.h"
#include "decoder.h"
#include "dir.h"
#include "download.h"
#include <gst/gst.h>
#include <stdbool.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        return -1;
    }
    gst_init(&argc, &argv);

    config_t config;
    read_config(argv[1], &config);

    sqlite3 *db;
    if (open_database_readonly(config.db, &db) != SQLITE_OK) {
        return -1;
    }

    config.num_tracks = count_tracks(db);

    const int width = 20;
    while (true) {
        file_info_t *infos =
            (file_info_t *)malloc(config.num_files * sizeof(file_info_t));
        if (!infos) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }

        int rc = delete_directory(config.output);
        if (rc != 0) {
            fprintf(stderr, "Failed to delete directory\n");
            exit(-1);
        }
        create_directory(config.output);

        download_init(infos, &config, db);
        download_files(infos, &config);
        assemble_files(infos, &config);

        for (int i = 0; i < config.num_files; ++i) {
            if (infos[i].download_status == DOWNLOAD_OK) {
                char *file_path =
                    get_file_path(config.output, infos[i].filename);

                fprintf(stdout, "%-*s: %s\n", width + 2, "PLAYING",
                        infos[i].filename);
                fprintf(stdout, "  %-*s: %s\n", width, "path",
                        infos[i].album_path);
                fprintf(stdout, "  %-*s: %s\n", width, "filename",
                        infos[i].track_name);

                decode_audio(&config, file_path);
                free(file_path);
            }
        }

        fprintf(stdout, "end-5z2ok9v4iik5tdykgms90qrc6\n");
        fflush(stdout);
        download_cleanup(infos, config.num_files);
    }

    sqlite3_close(db);
    free_config(&config);

    return 0;
}
