#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include "config.h"
#include "database.h"
#include "random.h"
#include <ctype.h>
#include <curl/curl.h>
#include <uv.h>

typedef struct {
    uv_work_t work_req;
    CURL *curl;
    char **cids;
    int num_cids;
    config_t *config;
    int retries;
    int cid_index;
    int *downloaded;
    int *completed;
    char *filename;
} cid_downloader_t;

typedef struct {
    char *filename;
    char *album_path;
    char *track_name;
    char *ext;
    char **cids;
    int num_cids;
    config_t *config;
    uv_loop_t *loop;
    cid_downloader_t *cid_tasks;
    uv_work_t work_req;
    int *downloaded;
    int *completed;
} file_downloader_t;

void to_lowercase(char *str);
void initialize_downloads(file_downloader_t *infos, int num_files,
                          config_t *config, sqlite3 *db);
void perform_downloads(file_downloader_t *infos, int num_files,
                       uv_loop_t *loop);
void cleanup_downloads(file_downloader_t *infos, int num_files);

#endif // DOWNLOADER_H
