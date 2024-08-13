#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "config.h"
#include "database.h"
#include "random.h"
#include "utils.h"

enum download_status { DOWNLOAD_PENDING, DOWNLOAD_OK, DOWNLOAD_FAILED };
typedef struct {
    enum download_status download_status;
} cid_info_t;

typedef struct {
    char *filename;
    char *album_path;
    char *track_name;
    char *ext;
    char **cids;
    int num_cids;
    config_t *config;
    cid_info_t *cid_infos;
    enum download_status download_status;
} file_info_t;

void download_cleanup(file_info_t *infos, int num_files);
void download_init(file_info_t *infos, config_t *config, sqlite3 *db);
void download_files(file_info_t *infos, config_t *config);
void assemble_files(file_info_t *infos, config_t *config);

#endif // DOWNLOAD_H
