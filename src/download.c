#include <stdlib.h>
#include <string.h>

#include <apr_strings.h>
#include <apr_thread_pool.h>
#include <apr_time.h>
#include <curl/curl.h>

#include "const.h"
#include "download.h"
#include "log.h"

typedef struct {
    char *cid;
    enum download_status *cid_download_status;
    config_t *config;
} download_info_t;

static void assemble_file(file_info_t *info, config_t *config);

apr_status_t file_info_free(void *data) {
    log_trace("file_info_free: start");
    file_info_t *info = (file_info_t *)data;
    free(info->track_name);
    free(info->album_path);
    free(info->filename);
    free(info->extension);
    for (int j = 0; j < info->num_cids; ++j) {
        free(info->cids[j]);
    }
    free(info->cids);
    free(info->cid_download_status);
    log_trace("file_info_free: finish");
    return APR_SUCCESS;
}

static FILE *open_file_write(const char *file_path) {
    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        log_trace("Failed to open file %s", file_path);
        exit(-1);
    }
    return fp;
}

static void set_curl_opts(CURL *curl, char *url, download_info_t *download_info,
                          int retries) {
    if (strlen(download_info->cid) == 59) {
        snprintf(url, 128, "https://%s.%s", download_info->cid,
                 download_info->config->n_gateway);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                         2 * download_info->config->timeout);
    } else {
        if (retries == 4 || retries == 5) {
            snprintf(url, 128, "https://%s/%s",
                     download_info->config->i_gateway, download_info->cid);
        } else {
            int *random_index =
                util_random_ints(1, 0, download_info->config->num_gateways - 1);
            snprintf(url, 128, "https://%s/%s",
                     download_info->config->gateways[*random_index],
                     download_info->cid);
            free(random_index);
        }
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, download_info->config->timeout);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    log_trace("download_cid: downloading from %s", url);
}

static size_t write_callback(void *ptr, size_t size, size_t nmemb,
                             FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static void perform_curl_download(CURL *curl, FILE *fp,
                                  download_info_t *download_info) {
    int retries = 0;
    CURLcode res;
    long response_code;
    char url[128];
    char *content_type = NULL;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    do {
        set_curl_opts(curl, url, download_info, retries);
        res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
            if (response_code == 200 &&
                (strlen(download_info->cid) == 59 ||
                 (content_type &&
                  strcmp(content_type, "application/octet-stream") == 0))) {

                *(download_info->cid_download_status) = DOWNLOAD_SUCCEEDED;
                log_trace("download_cid: finish downloading %s",
                          download_info->cid);
                fprintf(stdout, "Finish downloading %s\n", download_info->cid);
                break;
            }
        }

        log_trace("download_cid: Retry to download %s (attempt %d)",
                  download_info->cid, retries + 1);
        retries++;
        rewind(fp);
    } while (retries < download_info->config->max_retries);

    if (res != CURLE_OK || response_code != 200) {
        log_trace("download_cid: Download of cid %s failed after %d tries",
                  download_info->cid, retries + 1);
        *(download_info->cid_download_status) = DOWNLOAD_FAILED;
    }
}

static void *APR_THREAD_FUNC download_cid(apr_thread_t *thd, void *data) {
    download_info_t *download_info = (download_info_t *)data;

    fprintf(stdout, "Downloading %s\n", download_info->cid);
    log_trace("download_cid: start downloading %s", download_info->cid);
    fflush(stdout);

    CURL *curl = curl_easy_init();

    if (!curl) {
        log_trace("download_cid: Failed to create curl handle");
        exit(-1);
    }

    char *file_path =
        util_get_file_path(download_info->config->output, download_info->cid);
    FILE *fp = open_file_write(file_path);
    perform_curl_download(curl, fp, download_info);

    fclose(fp);
    free(file_path);
    curl_easy_cleanup(curl);
    return NULL;
}

static apr_thread_pool_t *create_pool(apr_pool_t *pool, config_t *config) {
    apr_thread_pool_t *thread_pool;
    apr_status_t status;

    apr_size_t ncores = 2;
    status = apr_thread_pool_create(&thread_pool, config->num_files, 4 * ncores,
                                    pool);
    if (status != APR_SUCCESS) {
        log_trace("Failed to create thread pool");
        exit(-1);
    }

    return thread_pool;
}

static void push_task(apr_thread_pool_t *thread_pool, file_info_t *info,
                      int cid_index, apr_pool_t *pool) {
    download_info_t *download_info = apr_palloc(pool, sizeof(download_info_t));
    download_info->cid = info->cids[cid_index];
    download_info->cid_download_status =
        &(info->cid_download_status[cid_index]);
    download_info->config = info->config;

    apr_status_t status =
        apr_thread_pool_push(thread_pool, download_cid, download_info, 0, NULL);
    if (status != APR_SUCCESS) {
        log_trace("push_task: Failed to push task to thread pool for cid %s",
                  info->cids[cid_index]);
        exit(-1);
    }
}

static void wait_tasks(apr_thread_pool_t *thread_pool) {
    while (apr_thread_pool_tasks_count(thread_pool) > 0) {
        apr_sleep(apr_time_from_sec(1));
    }
}

static int is_download_successful(file_info_t *info) {
    for (int j = 0; j < info->num_cids; ++j) {
        if (info->cid_download_status[j] != DOWNLOAD_SUCCEEDED) {
            info->file_download_status = DOWNLOAD_FAILED;
            return 0;
        }
    }
    info->file_download_status = DOWNLOAD_SUCCEEDED;
    return 1;
}

static void log_duration(apr_time_t start) {
    apr_time_t end = apr_time_now();
    apr_time_t diff_usec = end - start;
    double elapsed_time = (double)diff_usec / APR_USEC_PER_SEC;
    log_trace("Downloading took %.3f seconds", elapsed_time);
    fprintf(stdout, "%s %.3f seconds\n", "Downloading took", elapsed_time);
}

void download_assemble_files(apr_pool_t *pool, sqlite3 *db, config_t *config) {
    log_trace("download_files: start");

    for (int i = 0; i < 3; ++i) {
        log_trace("download_files: start loop");
        apr_pool_t *subpool;
        apr_pool_create(&subpool, pool);

        apr_thread_pool_t *thread_pool = create_pool(subpool, config);
        apr_time_t start = apr_time_now();

        file_info_t *info = apr_palloc(subpool, sizeof(file_info_t));
        int *random_index =
            util_random_ints(1, config->min_value, config->num_tracks);
        file_info_init(info, *random_index, db, config);
        free(random_index);
        apr_pool_cleanup_register(subpool, info, file_info_free,
                                  apr_pool_cleanup_null);

        for (int j = 0; j < info->num_cids; ++j) {
            push_task(thread_pool, info, j, subpool);
        }

        wait_tasks(thread_pool);
        log_duration(start);
        apr_thread_pool_destroy(thread_pool);
        apr_pool_destroy(subpool);

        if (is_download_successful(info)) {
            assemble_file(info, config);
        }
        log_trace("download_files: end loop");
    }

    log_trace("download_files: finish");
}

void file_info_init(file_info_t *info, int index, sqlite3 *db,
                    config_t *config) {
    int num_cids;

    info->track_name = database_get_track_name(db, index);
    info->album_path = database_get_album(db, index);
    info->filename = util_get_filename_with_extension(info->track_name);
    info->extension = util_get_extension(info->track_name);
    info->cids = database_get_cids(db, index, &num_cids);
    info->num_cids = num_cids;
    info->track_id = index;
    info->config = config;
    info->file_download_status = DOWNLOAD_PENDING;

    info->cid_download_status =
        (enum download_status *)malloc(num_cids * sizeof(enum download_status));
    if (!info->cid_download_status) {
        log_trace("Memory allocation failed");
        exit(-1);
    }

    for (int j = 0; j < info->num_cids; ++j) {
        info->cid_download_status[j] = DOWNLOAD_PENDING;
    }
}

static void append_cid_output(char *filename, char *cid, FILE *outfile,
                              char *buffer, config_t *config) {
    char *cid_path = util_get_file_path(config->output, cid);

    FILE *infile = fopen(cid_path, "rb");
    if (!infile) {
        log_trace("append_cid_output: Failed to open file %s", cid_path);
        exit(-1);
    }

    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, 4096, infile)) > 0) {
        fwrite(buffer, 1, bytes_read, outfile);
    }

    fclose(infile);
    log_trace("append_cid_output: %s -> %s", cid, filename);

    if (remove(cid_path) != 0) {
        log_trace("append_cid_output: Failed to delete file %s", cid_path);
        exit(-1);
    }

    free(cid_path);
}

static void assemble_multiple_cids(file_info_t *info, char *file_path,
                                   config_t *config) {
    FILE *outfile = fopen(file_path, "wb");
    if (!outfile) {
        log_trace("assemble_multiple_cids: Failed to open file %s", file_path);
        exit(-1);
    }

    char *buffer = (char *)malloc(4096);
    if (!buffer) {
        log_trace("assemble_multiple_cids: Memory allocation failed");
        exit(-1);
    }
    for (int j = 0; j < info->num_cids; j++) {
        append_cid_output(info->filename, info->cids[j], outfile, buffer,
                          config);
    }

    fclose(outfile);
    free(file_path);
    free(buffer);
}

static void log_assembly(file_info_t *info) {
    fprintf(stdout, "%-*s: %s\n", WIDTH + 2, "Assemble", info->filename);
    log_trace("assemble: track: %d / %d", info->track_id,
              info->config->num_tracks);
    fprintf(stdout, "  %-*s: %d / %d\n", WIDTH, "track", info->track_id,
            info->config->num_tracks);
    log_trace("assemble: path: %s", info->album_path);
    fprintf(stdout, "  %-*s: %s\n", WIDTH, "path", info->album_path);
    log_trace("assemble: filename: %s", info->track_name);
    fprintf(stdout, "  %-*s: %s\n", WIDTH, "filename", info->track_name);

    if (info->num_cids == 1) {
        log_trace("assemble: info: %s -> %s", info->cids[0], info->filename);
        fprintf(stdout, "  %-*s: %s -> %s\n", WIDTH, "info", info->cids[0],
                info->filename);
    } else {
        log_trace("assemble: info: %d CIDs -> %s", info->num_cids,
                  info->filename);
        fprintf(stdout, "  %-*s: %d CIDs -> %s\n", WIDTH, "info",
                info->num_cids, info->filename);
    }

    fprintf(stdout, "\n");
    fflush(stdout);
}

static void move_single_file(file_info_t *info, char *file_path,
                             config_t *config) {
    char *cid_path = util_get_file_path(config->output, info->cids[0]);

    if (rename(cid_path, file_path) != 0) {
        log_trace("move_single_file: Failed to move file %s to %s",
                  info->cids[0], info->filename);
        exit(-1);
    }

    log_trace("move_single_file: %s -> %s", info->cids[0], info->filename);
    free(cid_path);
    free(file_path);
}

static void assemble_file(file_info_t *info, config_t *config) {
    log_trace("assemble_file: start assembling %s", info->filename);
    log_assembly(info);
    char *file_path = util_get_file_path(config->output, info->filename);

    if (info->num_cids == 1) {
        move_single_file(info, file_path, config);
    } else {
        assemble_multiple_cids(info, file_path, config);
    }
    log_trace("assemble: finish assembling %s", info->filename);
}
