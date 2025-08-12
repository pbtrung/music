#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <apr_pools.h>

#include "config.h"
#include "database.h"
#include "utils.h"

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

void file_info_init(file_info_t *info, int index, sqlite3 *db,
                    config_t *config);
apr_status_t file_info_free(void *data);

void download_assemble_file(apr_pool_t *pool, config_t *config,
                            file_info_t *info);

#endif // DOWNLOAD_H