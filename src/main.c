#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// Security constants
#define MAX_CONFIG_PATH_LENGTH 4096
#define MAX_TASK_FIELD_LENGTH 512
#define MAX_QUEUE_SIZE_LIMIT 10000
#define MIN_QUEUE_SIZE 1
#define MAX_FORMATTED_NUMBER_LENGTH 32

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

// Input validation functions
static bool validate_config_path(const char *path) {
    if (!path) {
        log_trace("validate_config_path: NULL path");
        return false;
    }

    size_t len = strlen(path);
    if (len == 0 || len > MAX_CONFIG_PATH_LENGTH) {
        log_trace("validate_config_path: Invalid path length: %zu", len);
        return false;
    }

    // Check for directory traversal attempts
    if (strstr(path, "../") || strstr(path, "..\\")) {
        log_trace("validate_config_path: Directory traversal attempt detected");
        return false;
    }

    // Check if file exists and is readable
    if (access(path, R_OK) != 0) {
        log_trace("validate_config_path: File not readable: %s", path);
        return false;
    }

    return true;
}

static bool validate_task_field(const char *field, const char *field_name) {
    if (!field) {
        log_trace("validate_task_field: NULL %s", field_name);
        return false;
    }

    size_t len = strlen(field);
    if (len == 0 || len > MAX_TASK_FIELD_LENGTH) {
        log_trace("validate_task_field: Invalid %s length: %zu", field_name,
                  len);
        return false;
    }

    return true;
}

static char *safe_strdup(const char *str, const char *context) {
    if (!str) {
        log_trace("safe_strdup: NULL string in %s", context);
        return NULL;
    }

    size_t len = strlen(str);
    if (len > MAX_TASK_FIELD_LENGTH) {
        log_trace("safe_strdup: String too long in %s: %zu", context, len);
        return NULL;
    }

    char *dup = malloc(len + 1);
    if (!dup) {
        log_trace("safe_strdup: Memory allocation failed in %s", context);
        return NULL;
    }

    strcpy(dup, str);
    return dup;
}

static void exit_on_error(const char *msg) {
    if (msg) {
        fprintf(stderr, "Error: %s\n", msg);
        log_trace("Fatal error: %s", msg);
    }
    exit(-1);
}

static void initialize_pool(apr_pool_t **pool) {
    if (!pool) {
        exit_on_error("initialize_pool: NULL pool pointer");
    }

    apr_status_t status = apr_initialize();
    if (status != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(status, errbuf, sizeof(errbuf));
        log_trace("initialize_pool: APR initialization failed: %s", errbuf);
        exit_on_error("Failed to init APR");
    }

    status = apr_pool_create(pool, NULL);
    if (status != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(status, errbuf, sizeof(errbuf));
        log_trace("initialize_pool: Pool creation failed: %s", errbuf);
        exit_on_error("Failed to create APR pool");
    }
}

static config_t *load_config(const char *config_file) {
    if (!validate_config_path(config_file)) {
        exit_on_error("Invalid config file path");
    }

    config_t *cfg = malloc(sizeof(config_t));
    if (!cfg) {
        exit_on_error("Memory allocation failed for config");
    }

    // Initialize config structure to prevent use of uninitialized values
    memset(cfg, 0, sizeof(config_t));

    config_read(config_file, cfg);

    // Validate critical config values
    if (!cfg->output || strlen(cfg->output) == 0) {
        log_trace("load_config: Invalid output directory in config");
        free(cfg);
        exit_on_error("Invalid output directory in config");
    }

    if (!cfg->log || strlen(cfg->log) == 0) {
        log_trace("load_config: Invalid log path in config");
        free(cfg);
        exit_on_error("Invalid log path in config");
    }

    if (cfg->num_files <= 0 || cfg->num_files > MAX_QUEUE_SIZE_LIMIT) {
        log_trace("load_config: Invalid num_files: %d", cfg->num_files);
        free(cfg);
        exit_on_error("Invalid num_files in config");
    }

    return cfg;
}

static FILE *open_log_file(const char *path) {
    if (!path || strlen(path) == 0) {
        exit_on_error("Invalid log file path");
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        char error_msg[512];
        snprintf(error_msg, sizeof(error_msg),
                 "Failed to open log file: %s (errno: %d)", path, errno);
        exit_on_error(error_msg);
    }

    if (log_add_fp(fp, LOG_TRACE) != 0) {
        fclose(fp);
        exit_on_error("Failed to configure logging");
    }

    log_set_quiet(true);
    return fp;
}

static void free_file_task(file_task_t *task) {
    if (!task) {
        return;
    }

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

static file_info_t *prepare_file_info(apr_pool_t *subpool, config_t *cfg) {
    if (!subpool || !cfg) {
        log_trace("prepare_file_info: Invalid parameters");
        return NULL;
    }

    file_info_t *info = apr_palloc(subpool, sizeof(file_info_t));
    if (!info) {
        log_trace("prepare_file_info: Memory allocation failed");
        return NULL;
    }

    // Initialize the structure
    memset(info, 0, sizeof(file_info_t));

    json_t *doc = cosmosdb_get_item(subpool, cfg);
    if (!doc) {
        log_trace("prepare_file_info: Failed to get CosmosDB item");
        return NULL;
    }

    cosmosdb_file_info_init(info, doc, cfg);

    json_decref(doc);
    apr_pool_cleanup_register(subpool, info, file_info_free,
                              apr_pool_cleanup_null);

    return info;
}

static file_task_t *create_file_task(const file_info_t *info,
                                     const config_t *cfg) {
    if (!info || !cfg) {
        log_trace("create_file_task: Invalid parameters");
        return NULL;
    }

    // Validate required fields
    if (!validate_task_field(info->filename, "filename") ||
        !validate_task_field(cfg->pipe_name, "pipe_name") ||
        !validate_task_field(info->album_path, "album_path") ||
        !validate_task_field(info->track_name, "track_name")) {
        log_trace("create_file_task: Invalid task fields");
        return NULL;
    }

    if (!info->cids || info->num_cids <= 0 || !info->cids[0]) {
        log_trace("create_file_task: Invalid CID data");
        return NULL;
    }

    file_task_t *task = malloc(sizeof(file_task_t));
    if (!task) {
        log_trace("create_file_task: Failed malloc");
        return NULL;
    }

    // Initialize all fields to NULL first
    memset(task, 0, sizeof(file_task_t));

    // Use safe_strdup for all string fields
    task->filename = safe_strdup(info->filename, "filename");
    task->pipe_name = safe_strdup(cfg->pipe_name, "pipe_name");
    task->album_path = safe_strdup(info->album_path, "album_path");
    task->track_name = safe_strdup(info->track_name, "track_name");
    task->cid = safe_strdup(info->cids[0], "cid");

    // Get file path safely
    task->file_path = util_make_path(cfg->output, info->filename);

    // Validate integer fields
    if (info->track_id < 0 || cfg->max_value < 0 || info->num_cids <= 0) {
        log_trace("create_file_task: Invalid numeric values");
        free_file_task(task);
        return NULL;
    }

    task->track_id = info->track_id;
    task->num_tracks = cfg->max_value;
    task->num_cids = info->num_cids;

    // Check if all string allocations succeeded
    if (!task->filename || !task->pipe_name || !task->album_path ||
        !task->track_name || !task->cid || !task->file_path) {
        log_trace("create_file_task: Failed string allocation");
        free_file_task(task);
        return NULL;
    }

    return task;
}

static apr_status_t push_task_to_queue(apr_queue_t *q, file_task_t *task) {
    if (!q || !task) {
        log_trace("push_task_to_queue: Invalid parameters");
        return APR_EINVAL;
    }

    apr_status_t rv = apr_queue_push(q, task);
    if (rv != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(rv, errbuf, sizeof(errbuf));
        log_trace("push_task_to_queue: Failed to push %s: %s",
                  task->file_path ? task->file_path : "NULL", errbuf);

        // Attempt to clean up the file if it exists
        if (task->file_path && access(task->file_path, F_OK) == 0) {
            if (remove(task->file_path) != 0) {
                log_trace("push_task_to_queue: Failed to delete %s: errno %d",
                          task->file_path, errno);
            } else {
                log_trace("push_task_to_queue: Cleaned up file %s",
                          task->file_path);
            }
        }
    }
    return rv;
}

static void process_file(apr_pool_t *subpool, config_t *cfg, apr_queue_t *q) {
    if (!subpool || !cfg || !q) {
        log_trace("process_file: Invalid parameters");
        return;
    }

    file_info_t *info = prepare_file_info(subpool, cfg);
    if (!info) {
        log_trace("process_file: Failed to prepare file info");
        return;
    }

    download_assemble_file(subpool, cfg, info);

    if (info->file_download_status != DOWNLOAD_SUCCEEDED) {
        log_trace("process_file: Download failed");
        return;
    }

    file_task_t *task = create_file_task(info, cfg);
    if (!task) {
        log_trace("process_file: Failed to create file task");
        exit(-1);
    }

    if (push_task_to_queue(q, task) != APR_SUCCESS) {
        free_file_task(task);
        exit(-1);
    }

    log_trace("process_file: queued %s", task->filename);
}

static void run_downloader(apr_pool_t *pool, const char *cfg_file,
                           config_t **cfg, apr_queue_t *q) {
    if (!pool || !cfg_file || !cfg || !q) {
        log_trace("run_downloader: Invalid parameters");
        return;
    }

    while (true) {
        log_trace("downloader_thread: start loop");

        apr_pool_t *subpool;
        apr_status_t status = apr_pool_create(&subpool, pool);
        if (status != APR_SUCCESS) {
            char errbuf[256];
            apr_strerror(status, errbuf, sizeof(errbuf));
            log_trace("run_downloader: Failed to create subpool: %s", errbuf);
            continue;
        }

        *cfg = apr_palloc(subpool, sizeof(config_t));
        if (!*cfg) {
            log_trace("run_downloader: Failed to allocate config");
            apr_pool_destroy(subpool);
            continue;
        }

        memset(*cfg, 0, sizeof(config_t));

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

    if (!data) {
        log_trace("downloader_thread: NULL data");
        return NULL;
    }

    downloader_args_t *args = (downloader_args_t *)data;
    if (!args->pool || !args->config_file || !args->config || !args->queue) {
        log_trace("downloader_thread: Invalid args");
        return NULL;
    }

    run_downloader(args->pool, args->config_file, args->config, args->queue);
    log_trace("downloader_thread: end");
    return NULL;
}

static void safe_print_task_info(const file_task_t *task) {
    if (!task) {
        log_trace("safe_print_task_info: NULL task");
        return;
    }

    char track_id_str[MAX_FORMATTED_NUMBER_LENGTH] = "0";
    char num_tracks_str[MAX_FORMATTED_NUMBER_LENGTH] = "0";

    // Safe formatting with bounds checking
    if (util_format_commas(task->track_id, track_id_str,
                           sizeof(track_id_str)) == NULL) {
        snprintf(track_id_str, sizeof(track_id_str), "%d", task->track_id);
    }

    if (util_format_commas(task->num_tracks, num_tracks_str,
                           sizeof(num_tracks_str)) == NULL) {
        snprintf(num_tracks_str, sizeof(num_tracks_str), "%d",
                 task->num_tracks);
    }

    fprintf(stdout, "PLAYING: %s\n",
            task->filename ? task->filename : "UNKNOWN");
    fprintf(stdout, "  %-*s: %s / %s\n", WIDTH, "track", track_id_str,
            num_tracks_str);
    fprintf(stdout, "  %-*s: %s\n", WIDTH, "album",
            task->album_path ? task->album_path : "UNKNOWN");
    fprintf(stdout, "  %-*s: %s\n", WIDTH, "filename",
            task->track_name ? task->track_name : "UNKNOWN");

    if (task->num_cids == 1) {
        fprintf(stdout, "  %-*s: %s -> %s\n", WIDTH, "info",
                task->cid ? task->cid : "UNKNOWN",
                task->filename ? task->filename : "UNKNOWN");
    } else {
        fprintf(stdout, "  %-*s: %d CIDs -> %s\n", WIDTH, "info",
                task->num_cids, task->filename ? task->filename : "UNKNOWN");
    }

    fflush(stdout);
}

static void consume_files(apr_queue_t *queue) {
    if (!queue) {
        log_trace("consume_files: NULL queue");
        return;
    }

    while (true) {
        file_task_t *task = NULL;
        apr_status_t rv = apr_queue_pop(queue, (void **)&task);

        if (rv == APR_EOF) {
            log_trace("consume_files: Queue closed");
            break;
        }

        if (rv != APR_SUCCESS) {
            char errbuf[256];
            apr_strerror(rv, errbuf, sizeof(errbuf));
            log_trace("consume_files: Queue pop failed: %s", errbuf);
            continue;
        }

        if (!task) {
            log_trace("consume_files: NULL task from queue");
            continue;
        }

        if (!task->file_path || !task->filename) {
            log_trace("consume_files: Invalid task data");
            free_file_task(task);
            continue;
        }

        log_trace("main: queue_pop: filename: %s", task->filename);
        log_trace("main: start decode_audio: %s", task->filename);

        safe_print_task_info(task);

        // Validate required fields before calling decode_audio
        if (!task->pipe_name) {
            log_trace("consume_files: Missing pipe_name");
            free_file_task(task);
            continue;
        }

        decode_audio(task->pipe_name, task->filename, task->file_path);

        log_trace("main: end decode_audio: %s", task->filename);

        // Clean up the temporary file
        if (access(task->file_path, F_OK) == 0) {
            if (remove(task->file_path) != 0) {
                log_trace("main: Failed to delete file %s: errno %d",
                          task->file_path, errno);
                exit(-1);
            } else {
                log_trace("main: Successfully deleted file %s",
                          task->file_path);
            }
        }

        free_file_task(task);
    }
}

static apr_queue_t *create_safe_queue(apr_pool_t *pool, int capacity) {
    if (!pool) {
        log_trace("create_safe_queue: NULL pool");
        return NULL;
    }

    if (capacity < MIN_QUEUE_SIZE || capacity > MAX_QUEUE_SIZE_LIMIT) {
        log_trace("create_safe_queue: Invalid capacity: %d", capacity);
        return NULL;
    }

    apr_queue_t *queue;
    apr_status_t status = apr_queue_create(&queue, capacity, pool);
    if (status != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(status, errbuf, sizeof(errbuf));
        log_trace("create_safe_queue: Failed to create queue: %s", errbuf);
        return NULL;
    }

    return queue;
}

int main(int argc, const char *argv[]) {
    // Input validation
    if (argc != 2) {
        exit_on_error("Usage: <program> <config_file>");
    }

    if (!argv[1]) {
        exit_on_error("Config file argument is NULL");
    }

    apr_pool_t *pool = NULL;
    FILE *fp = NULL;
    config_t *cfg = NULL;

    initialize_pool(&pool);

    cfg = load_config(argv[1]);
    fp = open_log_file(cfg->log);

    log_trace("main: start");

    dir_delete(pool, cfg->output);
    dir_create(pool, cfg->output);

    apr_queue_t *queue = create_safe_queue(pool, cfg->num_files);
    if (!queue) {
        fclose(fp);
        config_free(cfg);
        free(cfg);
        apr_pool_destroy(pool);
        apr_terminate();
        exit_on_error("Failed to create APR queue");
    }

    // Clean up initial config since it will be reloaded in thread
    config_free(cfg);
    free(cfg);
    cfg = NULL;

    // Thread creation with error checking
    apr_thread_t *dl_thread;
    apr_threadattr_t *dl_attr;
    apr_status_t status = apr_threadattr_create(&dl_attr, pool);
    if (status != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(status, errbuf, sizeof(errbuf));
        log_trace("main: Failed to create thread attributes: %s", errbuf);
        fclose(fp);
        apr_pool_destroy(pool);
        apr_terminate();
        exit_on_error("Failed to create thread attributes");
    }

    downloader_args_t dl_args = {
        .queue = queue, .pool = pool, .config_file = argv[1], .config = &cfg};

    log_trace("main: start downloader_thread");
    status = apr_thread_create(&dl_thread, dl_attr, downloader_thread, &dl_args,
                               pool);
    if (status != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(status, errbuf, sizeof(errbuf));
        log_trace("main: Failed to create downloader thread: %s", errbuf);
        fclose(fp);
        apr_pool_destroy(pool);
        apr_terminate();
        exit_on_error("Failed to create downloader thread");
    }

    log_trace("main: start consume_files");
    consume_files(queue);

    log_trace("main: start apr_thread_join");
    apr_status_t thread_rv;
    status = apr_thread_join(&thread_rv, dl_thread);
    if (status != APR_SUCCESS) {
        char errbuf[256];
        apr_strerror(status, errbuf, sizeof(errbuf));
        log_trace("main: Thread join failed: %s", errbuf);
    }

    log_trace("main: end");

    // Cleanup
    if (fp) {
        fclose(fp);
    }

    if (pool) {
        apr_pool_destroy(pool);
    }

    apr_terminate();
    return 0;
}