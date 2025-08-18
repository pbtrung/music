#ifndef FILE_INFO_H
#define FILE_INFO_H

#include <apr_pools.h>
#include <jansson.h>

#include "config.h"
#include "download.h"

enum download_status { DOWNLOAD_PENDING, DOWNLOAD_SUCCEEDED, DOWNLOAD_FAILED };

typedef struct {
    char *filename;
    char *album_path;
    char *track_name;
    char *extension;
    char **cids;
    int num_cids;
    int track_id;
    config_t *config;
    enum download_status *cid_download_status;
    enum download_status file_download_status;
} file_info_t;

file_info_t *file_info_create(apr_pool_t *subpool, config_t *cfg, json_t *doc);
apr_status_t file_info_free(void *data);

#endif // FILE_INFO_H