#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <apr_pools.h>
#include <apr_queue.h>
#include <apr_thread_proc.h>

#include "config.h"
#include "const.h"
#include "cosmosdb.h"
#include "decode.h"
#include "dir.h"
#include "download.h"
#include "log.h"
#include "utils.h"

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

typedef struct {
    apr_queue_t *queue;
    apr_pool_t *pool;
    const char *config_file;
    config_t **config;
} downloader_args_t;

static void exit_on_error(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(-1);
}

static void initialize_pool(apr_pool_t **pool) {
    if (apr_initialize() != APR_SUCCESS)
        exit_on_error("Failed to init APR");

    apr_pool_create(pool, NULL);
}

static config_t *load_config(const char *config_file) {
    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg)
        exit_on_error("Memory allocation failed");

    config_read(config_file, cfg);
    return cfg;
}

static FILE *open_log_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", path);
        exit(-1);
    }
    log_add_fp(fp, LOG_TRACE);
    log_set_quiet(true);
    return fp;
}

static void free_file_task(file_task_t *task) {
    if (!task)
        return;
    free(task->filename);
    free(task->pipe_name);
    free(task->file_path);
    free(task->album_path);
    free(task->track_name);
    free(task->cid);
    free(task);
}

static file_info_t *prepare_file_info(apr_pool_t *subpool, config_t *cfg) {
    file_info_t *info = apr_palloc(subpool, sizeof(file_info_t));
    json_t *doc = cosmosdb_get_item(subpool, cfg);
    cosmosdb_file_info_init(info, doc, cfg);
    json_decref(doc);
    return info;
}

static file_task_t *create_file_task(const file_info_t *info,
                                     const config_t *cfg) {
    file_task_t *task = malloc(sizeof(file_task_t));
    if (!task) {
        log_trace("create_file_task: Failed malloc");
        return NULL;
    }

    task->filename = strdup(info->filename);
    task->pipe_name = strdup(cfg->pipe_name);
    task->album_path = strdup(info->album_path);
    task->track_name = strdup(info->track_name);
    task->cid = strdup(info->cids[0]);
    task->file_path = util_get_file_path(cfg->output, info->filename);

    task->track_id = info->track_id;
    task->num_tracks = cfg->max_value;
    task->num_cids = info->num_cids;

    if (!task->filename || !task->pipe_name || !task->album_path ||
        !task->track_name || !task->cid || !task->file_path) {
        log_trace("create_file_task: Failed strdup");
        free_file_task(task);
        return NULL;
    }

    return task;
}

static apr_status_t push_task_to_queue(apr_queue_t *q, file_task_t *task) {
    apr_status_t rv = apr_queue_push(q, task);
    if (rv != APR_SUCCESS) {
        log_trace("push_task_to_queue: Failed to push %s", task->file_path);
        if (remove(task->file_path) != 0) {
            log_trace("push_task_to_queue: Failed to delete %s",
                      task->file_path);
        }
    }
    return rv;
}

static void process_file(apr_pool_t *subpool, config_t *cfg, apr_queue_t *q) {
    file_info_t *info = prepare_file_info(subpool, cfg);
    download_assemble_file(subpool, cfg, info);

    if (info->file_download_status != DOWNLOAD_SUCCEEDED)
        return;

    file_task_t *task = create_file_task(info, cfg);
    if (!task)
        exit(-1);

    if (push_task_to_queue(q, task) != APR_SUCCESS)
        exit(-1);

    log_trace("process_file: queued %s", task->filename);
}

static void run_downloader(apr_pool_t *pool, const char *cfg_file,
                           config_t **cfg, apr_queue_t *q) {
    while (true) {
        log_trace("downloader_thread: start loop");

        apr_pool_t *subpool;
        apr_pool_create(&subpool, pool);

        *cfg = apr_palloc(subpool, sizeof(config_t));
        config_read(cfg_file, *cfg);
        apr_pool_cleanup_register(subpool, *cfg, config_free,
                                  apr_pool_cleanup_null);
        (*cfg)->num_tracks = (*cfg)->max_value;

        process_file(subpool, *cfg, q);

        apr_pool_destroy(subpool);
        log_trace("downloader_thread: end loop");
    }
}

static void *APR_THREAD_FUNC downloader_thread(apr_thread_t *thd, void *data) {
    log_trace("downloader_thread: start");
    downloader_args_t *args = data;
    run_downloader(args->pool, args->config_file, args->config, args->queue);
    log_trace("downloader_thread: end");
    return NULL;
}

static void consume_files(apr_queue_t *queue) {
    while (true) {
        file_task_t *task = NULL;
        apr_status_t rv = apr_queue_pop(queue, (void **)&task);

        if (rv == APR_EOF)
            break;
        if (rv != APR_SUCCESS || !task || !task->file_path)
            continue;

        log_trace("main: queue_pop: filename: %s", task->filename);
        log_trace("main: start decode_audio: %s", task->filename);

        char track_id[16], num_tracks[16];
        format_number_commas(task->track_id, track_id, sizeof(track_id));
        format_number_commas(task->num_tracks, num_tracks, sizeof(num_tracks));

        fprintf(stdout, "PLAYING: %s\n", task->filename);
        fprintf(stdout, "  %-*s: %s / %s\n", WIDTH, "track", track_id,
                num_tracks);
        fprintf(stdout, "  %-*s: %s\n", WIDTH, "album", task->album_path);
        fprintf(stdout, "  %-*s: %s\n", WIDTH, "filename", task->track_name);
        if (task->num_cids == 1)
            fprintf(stdout, "  %-*s: %s -> %s\n", WIDTH, "info", task->cid,
                    task->filename);
        else
            fprintf(stdout, "  %-*s: %d CIDs -> %s\n", WIDTH, "info",
                    task->num_cids, task->filename);

        decode_audio(task->pipe_name, task->filename, task->file_path);
        log_trace("main: end decode_audio: %s", task->filename);

        if (remove(task->file_path) != 0) {
            log_trace("main: Failed to delete file %s", task->file_path);
            exit(-1);
        }

        free_file_task(task);
    }
}

int main(int argc, const char *argv[]) {
    if (argc != 2)
        exit_on_error("Usage: <program> <config_file>");

    apr_pool_t *pool;
    initialize_pool(&pool);

    config_t *cfg = load_config(argv[1]);
    FILE *fp = open_log_file(cfg->log);

    log_trace("main: start");

    dir_delete(pool, cfg->output);
    dir_create(pool, cfg->output);

    apr_queue_t *queue;
    if (apr_queue_create(&queue, cfg->num_files, pool) != APR_SUCCESS)
        exit_on_error("Failed to create APR queue");

    config_free(cfg);
    free(cfg);
    cfg = NULL;

    apr_thread_t *dl_thread;
    apr_threadattr_t *dl_attr;
    apr_threadattr_create(&dl_attr, pool);

    downloader_args_t dl_args = {
        .queue = queue, .pool = pool, .config_file = argv[1], .config = &cfg};

    log_trace("main: start downloader_thread");
    apr_thread_create(&dl_thread, dl_attr, downloader_thread, &dl_args, pool);

    log_trace("main: start consume_files");
    consume_files(queue);

    log_trace("main: start apr_thread_join");
    apr_thread_join(NULL, dl_thread);

    log_trace("main: end");
    fclose(fp);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}
