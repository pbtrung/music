#include "config.h"
#include "database.h"
#include "dir.h"
#include "downloader.h"
#include "random.h"
#include "track.h"
#include <libwebsockets.h>
#include <stdbool.h>
#include <string.h>
#include <uv.h>

int lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                 void *in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        lwsl_user("Connection established\n");
        break;

    case LWS_CALLBACK_RECEIVE:
        lwsl_user("Received message: %.*s\n", (int)len, (char *)in);

        // Echo the received message back to the client
        if (lws_write(wsi, (unsigned char *)in, len, LWS_WRITE_TEXT) < 0) {
            lwsl_err("Error writing to client\n");
        }
        break;

    case LWS_CALLBACK_CLOSED:
        lwsl_user("Connection closed\n");
        break;

    default:
        break;
    }

    return 0;
}

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

        // struct lws_context_creation_info lws_info;
        // struct lws_context *context;
        // struct lws_protocols protocols[] = {{"http", lws_callback, 0, 0},
        //                                     {NULL, NULL, 0, 0}};
        // memset(&lws_info, 0, sizeof(lws_info));
        // lws_info.port = 9000;
        // lws_info.protocols = protocols;
        // lws_info.options = LWS_SERVER_OPTION_LIBUV;
        // lws_info.user = loop;
        // context = lws_create_context(&lws_info);
        // if (context == NULL) {
        //     lwsl_err("Creating context failed\n");
        //     exit(-1);
        // }

        char *json = track_extract_metadata(infos, config.num_files);

        uv_work_t *work_req = malloc(sizeof(uv_work_t));
        if (!work_req) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        work_req->data = infos;
        uv_queue_work(loop, work_req, track_decode, NULL);

        uv_run(loop, UV_RUN_DEFAULT);

        cleanup_downloads(infos, config.num_files);
        free(infos);
        free(json);
        free(work_req);
        // lws_context_destroy(context);

        fprintf(stdout, "Sleeping...\n");
        sleep(2);
    }

    uv_loop_close(loop);
    sqlite3_close(db);
    free_config(&config);

    return 0;
}
