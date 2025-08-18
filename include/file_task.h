#ifndef FILE_TASK_H
#define FILE_TASK_H

#include "config.h"
#include "download.h"

typedef struct {
    char *filename;
    char *pipe_name;
    char *file_path;
    char *album_path;
    char *track_name;
    char *cid;
    int track_id;
    int num_tracks;
    int num_cids;
} file_task_t;

void file_task_free(file_task_t *task);
file_task_t *file_task_create(const file_info_t *info, const config_t *cfg);

#endif // FILE_TASK_H