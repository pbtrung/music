#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "config.h"
#include "database.h"
#include "util.h"
#include <apr_pools.h>

enum download_status { DOWNLOAD_PENDING, DOWNLOAD_SUCCEEDED, DOWNLOAD_FAILED };

typedef struct {
    char *filename;
    char *album_path;
    char *track_name;
    char *extension;
    char **cids;
    int num_cids;
    config_t *config;
    enum download_status *cid_download_status;
    enum download_status file_download_status;
} file_info_t;

typedef struct {
    file_info_t *file_infos;
    int num_files;
} file_infos_t;

apr_status_t download_cleanup(void *data);
void download_init(file_info_t *infos, config_t *config, sqlite3 *db);
void download_files(apr_pool_t *pool, file_info_t *infos, config_t *config);
void assemble_files(file_info_t *infos, config_t *config);

#endif // DOWNLOAD_H
