#include <stdlib.h>
#include <string.h>

#include "file_task.h"
#include "utils.h"

void file_task_free(file_task_t *task) {
    if (!task)
        return;

    free(task->filename);
    free(task->pipe_name);
    free(task->file_path);
    free(task->album_path);
    free(task->track_name);
    free(task->cid);

    // Clear the structure to prevent use-after-free bugs
    memset(task, 0, sizeof(file_task_t));
    free(task);
}

file_task_t *file_task_create(const file_info_t *info,
                                     const config_t *cfg) {
    if (!info || !cfg) {
        log_trace("file_task_create: Invalid parameters");
        return NULL;
    }

    // Validate required fields
    if (!validate_task_field(info->filename, "filename") ||
        !validate_task_field(cfg->pipe_name, "pipe_name") ||
        !validate_task_field(info->album_path, "album_path") ||
        !validate_task_field(info->track_name, "track_name")) {
        log_trace("file_task_create: Invalid task fields");
        return NULL;
    }

    if (!info->cids || info->num_cids <= 0 || !info->cids[0]) {
        log_trace("file_task_create: Invalid CID data");
        return NULL;
    }

    file_task_t *task = malloc(sizeof(file_task_t));
    if (!task) {
        log_trace("file_task_create: Failed malloc");
        return NULL;
    }

    // Initialize all fields to NULL first
    memset(task, 0, sizeof(file_task_t));

    task->filename = util_safe_strdup(info->filename, "filename");
    task->pipe_name = util_safe_strdup(cfg->pipe_name, "pipe_name");
    task->album_path = util_safe_strdup(info->album_path, "album_path");
    task->track_name = util_safe_strdup(info->track_name, "track_name");
    task->cid = util_safe_strdup(info->cids[0], "cid");

    // Get file path safely
    task->file_path = util_make_path(cfg->output, info->filename);

    // Validate integer fields
    if (info->track_id < 0 || cfg->max_value < 0 || info->num_cids <= 0) {
        log_trace("file_task_create: Invalid numeric values");
        file_task_free(task);
        return NULL;
    }

    task->track_id = info->track_id;
    task->num_tracks = cfg->max_value;
    task->num_cids = info->num_cids;

    // Check if all string allocations succeeded
    if (!task->filename || !task->pipe_name || !task->album_path ||
        !task->track_name || !task->cid || !task->file_path) {
        log_trace("file_task_create: Failed string allocation");
        file_task_free(task);
        return NULL;
    }

    return task;
}
