#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <apr_strings.h>
#include <apr_thread_pool.h>
#include <apr_time.h>
#include <curl/curl.h>

#include "const.h"
#include "download.h"
#include "log.h"

// Security constants
#define MAX_CID_LENGTH 128
#define MAX_GATEWAY_LENGTH 256
#define MAX_URL_LENGTH 512
#define BUFFER_SIZE 4096
#define CID_LENGTH 59

typedef struct {
    char *cid;
    enum download_status *cid_download_status;
    config_t *config;
} download_info_t;

static void assemble_file(file_info_t *info, config_t *config);

// Security: Input validation functions
static bool validate_cid(const char *cid) {
    if (!cid) {
        log_trace("validate_cid: NULL CID");
        return false;
    }

    size_t len = strlen(cid);
    if (len == 0 || len > MAX_CID_LENGTH) {
        log_trace("validate_cid: Invalid CID length: %zu", len);
        return false;
    }

    // Check for valid characters only (alphanumeric, hyphens, underscores)
    for (const char *p = cid; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_') {
            log_trace("validate_cid: Invalid character in CID: %c", *p);
            return false;
        }
    }

    return true;
}

static bool validate_gateway(const char *gateway) {
    if (!gateway) {
        log_trace("validate_gateway: NULL gateway");
        return false;
    }

    size_t len = strlen(gateway);
    if (len == 0 || len > MAX_GATEWAY_LENGTH) {
        log_trace("validate_gateway: Invalid gateway length: %zu", len);
        return false;
    }

    // Basic domain name validation
    for (const char *p = gateway; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '-') {
            log_trace("validate_gateway: Invalid character in gateway: %c", *p);
            return false;
        }
    }

    return true;
}

apr_status_t file_info_free(void *data) {
    log_trace("file_info_free: start");
    if (!data) {
        log_trace("file_info_free: NULL data pointer");
        return APR_SUCCESS;
    }

    file_info_t *info = (file_info_t *)data;
    free(info->track_name);
    free(info->album_path);
    free(info->filename);
    free(info->extension);

    if (info->cids) {
        for (int j = 0; j < info->num_cids; ++j) {
            free(info->cids[j]);
        }
        free(info->cids);
    }
    free(info->cid_download_status);
    log_trace("file_info_free: finish");
    return APR_SUCCESS;
}

static FILE *open_file_write(const char *file_path) {
    if (!file_path) {
        log_trace("open_file_write: NULL file_path");
        exit(-1);
    }

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        log_trace("Failed to open file %s", file_path);
        exit(-1);
    }
    return fp;
}

static char *build_safe_url(const char *base, const char *cid,
                            bool is_subdomain) {
    if (!validate_cid(cid) || !validate_gateway(base)) {
        log_trace("build_safe_url: Invalid input parameters");
        return NULL;
    }

    size_t url_len = strlen(base) + strlen(cid) +
                     20; // Extra space for protocol and separators
    if (url_len > MAX_URL_LENGTH) {
        log_trace("build_safe_url: URL too long");
        return NULL;
    }

    char *url = malloc(url_len);
    if (!url) {
        log_trace("build_safe_url: Memory allocation failed");
        exit(-1);
    }

    if (is_subdomain) {
        snprintf(url, url_len, "https://%s.%s", cid, base);
    } else {
        snprintf(url, url_len, "https://%s/%s", base, cid);
    }

    return url;
}

static void set_curl_opts(CURL *curl, download_info_t *download_info,
                          int retries) {
    if (!curl || !download_info || !download_info->cid ||
        !download_info->config) {
        log_trace("set_curl_opts: Invalid parameters");
        exit(-1);
    }

    char *url = NULL;

    if (strlen(download_info->cid) == CID_LENGTH) {
        url = build_safe_url(download_info->config->n_gateway,
                             download_info->cid, true);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                         2 * download_info->config->timeout);
    } else {
        const char *gateway;
        if (retries == 4 || retries == 5) {
            gateway = download_info->config->i_gateway;
        } else {
            int *random_index =
                util_rand_ints(1, 0, download_info->config->num_gateways - 1);
            if (!random_index) {
                log_trace("set_curl_opts: Failed to generate random index");
                exit(-1);
            }
            gateway = download_info->config->gateways[*random_index];
            free(random_index);
        }
        url = build_safe_url(gateway, download_info->cid, false);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, download_info->config->timeout);
    }

    if (!url) {
        log_trace("set_curl_opts: Failed to build URL");
        exit(-1);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    log_trace("download_cid: downloading from %s", url);
    free(url);
}

static size_t write_callback(void *ptr, size_t size, size_t nmemb,
                             FILE *stream) {
    if (!ptr || !stream)
        return 0;
    return fwrite(ptr, size, nmemb, stream);
}

static void perform_curl_download(CURL *curl, FILE *fp,
                                  download_info_t *download_info) {
    if (!curl || !fp || !download_info) {
        log_trace("perform_curl_download: Invalid parameters");
        exit(-1);
    }

    int retries = 0;
    CURLcode res;
    long response_code;
    char *content_type = NULL;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    do {
        set_curl_opts(curl, download_info, retries);
        res = curl_easy_perform(curl);

        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
            if (response_code == 200 &&
                (strlen(download_info->cid) == CID_LENGTH ||
                 (content_type &&
                  strcmp(content_type, "application/octet-stream") == 0))) {

                *(download_info->cid_download_status) = DOWNLOAD_SUCCEEDED;
                log_trace("download_cid: finish downloading %s",
                          download_info->cid);
                break;
            }
        }

        log_trace("download_cid: Retry to download %s (attempt %d)",
                  download_info->cid, retries + 1);
        retries++;
        if (fseek(fp, 0, SEEK_SET) != 0) {
            log_trace("perform_curl_download: Failed to rewind file");
            exit(-1);
        }
    } while (retries < download_info->config->max_retries);

    if (res != CURLE_OK || response_code != 200) {
        log_trace("download_cid: Download of cid %s failed after %d tries",
                  download_info->cid, retries);
        *(download_info->cid_download_status) = DOWNLOAD_FAILED;
    }
}

static void *APR_THREAD_FUNC download_cid(apr_thread_t *thd, void *data) {
    download_info_t *download_info = (download_info_t *)data;
    if (!download_info) {
        log_trace("download_cid: NULL download_info");
        return NULL;
    }

    log_trace("download_cid: start downloading %s", download_info->cid);
    fflush(stdout);

    CURL *curl = curl_easy_init();
    if (!curl) {
        log_trace("download_cid: Failed to create curl handle");
        exit(-1);
    }

    char *file_path =
        util_make_path(download_info->config->output, download_info->cid);
    if (!file_path) {
        log_trace("download_cid: Failed to get file path");
        curl_easy_cleanup(curl);
        exit(-1);
    }

    FILE *fp = open_file_write(file_path);
    perform_curl_download(curl, fp, download_info);

    fclose(fp);
    free(file_path);
    curl_easy_cleanup(curl);
    log_trace("download_cid: end downloading %s", download_info->cid);
    return NULL;
}

static apr_thread_pool_t *create_pool(apr_pool_t *pool, config_t *config) {
    if (!pool || !config) {
        log_trace("create_pool: Invalid parameters");
        exit(-1);
    }

    apr_thread_pool_t *thread_pool;
    apr_status_t status;

    status = apr_thread_pool_create(&thread_pool, config->num_files,
                                    config->mul_factor * config->ncores, pool);
    if (status != APR_SUCCESS) {
        log_trace("Failed to create thread pool");
        exit(-1);
    }

    return thread_pool;
}

static void push_task(apr_thread_pool_t *thread_pool, file_info_t *info,
                      int cid_index, apr_pool_t *pool) {
    if (!thread_pool || !info || cid_index < 0 || cid_index >= info->num_cids) {
        log_trace("push_task: Invalid parameters");
        exit(-1);
    }

    log_trace("push_task: start %s", info->cids[cid_index]);
    download_info_t *download_info = apr_palloc(pool, sizeof(download_info_t));
    if (!download_info) {
        log_trace("push_task: Memory allocation failed");
        exit(-1);
    }

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
    log_trace("push_task: end %s", info->cids[cid_index]);
}

static int is_download_successful(file_info_t *info) {
    if (!info) {
        log_trace("is_download_successful: NULL info");
        return 0;
    }

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
}

static void delete_failed_file(file_info_t *info, config_t *config) {
    if (!info || !config) {
        log_trace("delete_failed_file: Invalid parameters");
        return;
    }

    for (int j = 0; j < info->num_cids; j++) {
        char *cid_path = util_make_path(config->output, info->cids[j]);
        if (!cid_path) {
            log_trace("delete_failed_file: Failed to get path for CID %s",
                      info->cids[j]);
            continue;
        }

        if (access(cid_path, F_OK) == 0) {
            if (remove(cid_path) != 0) {
                log_trace("delete_failed_file: Failed to delete file %s",
                          cid_path);
                exit(-1);
            }
            log_trace("delete_failed_file: Deleted file %s", cid_path);
        } else {
            log_trace("delete_failed_file: File does not exist, skipping %s",
                      cid_path);
        }

        free(cid_path);
    }
}

static void wait_tasks(file_info_t *info) {
    if (!info) {
        log_trace("wait_tasks: NULL info");
        return;
    }

    while (true) {
        bool completed = true;
        for (int j = 0; j < info->num_cids; ++j) {
            if (info->cid_download_status[j] == DOWNLOAD_PENDING) {
                completed = false;
                break;
            }
        }
        if (completed) {
            break;
        }
        apr_sleep(apr_time_from_sec(1));
    }
}

void download_assemble_file(apr_pool_t *pool, config_t *config,
                            file_info_t *info) {
    if (!pool || !config || !info) {
        log_trace("download_assemble_file: Invalid parameters");
        exit(-1);
    }

    log_trace("download_assemble_file: start");

    apr_pool_t *subpool;
    apr_status_t status = apr_pool_create(&subpool, pool);
    if (status != APR_SUCCESS) {
        log_trace("download_assemble_file: Failed to create subpool");
        exit(-1);
    }

    apr_thread_pool_t *thread_pool = create_pool(subpool, config);
    apr_time_t start = apr_time_now();

    log_trace("download_assemble_file: downloading %d cid(s)", info->num_cids);
    for (int j = 0; j < info->num_cids; ++j) {
        push_task(thread_pool, info, j, subpool);
    }

    wait_tasks(info);
    log_duration(start);

    status = apr_thread_pool_destroy(thread_pool);
    if (status != APR_SUCCESS) {
        log_trace(
            "download_assemble_file: Warning - failed to destroy thread pool");
    }

    if (is_download_successful(info)) {
        assemble_file(info, config);
    } else {
        delete_failed_file(info, config);
    }

    apr_pool_destroy(subpool);
    log_trace("download_assemble_file: finish");
}

static void append_cid_output(char *filename, char *cid, FILE *outfile,
                              char *buffer, config_t *config) {
    if (!filename || !cid || !outfile || !buffer || !config) {
        log_trace("append_cid_output: Invalid parameters");
        exit(-1);
    }

    char *cid_path = util_make_path(config->output, cid);
    if (!cid_path) {
        log_trace("append_cid_output: Failed to get file path");
        exit(-1);
    }

    FILE *infile = fopen(cid_path, "rb");
    if (!infile) {
        log_trace("append_cid_output: Failed to open file %s", cid_path);
        exit(-1);
    }

    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, infile)) > 0) {
        if (fwrite(buffer, 1, bytes_read, outfile) != bytes_read) {
            log_trace("append_cid_output: Failed to write to output file");
            exit(-1);
        }
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
    if (!info || !file_path || !config) {
        log_trace("assemble_multiple_cids: Invalid parameters");
        exit(-1);
    }

    FILE *outfile = fopen(file_path, "wb");
    if (!outfile) {
        log_trace("assemble_multiple_cids: Failed to open file %s", file_path);
        exit(-1);
    }

    char *buffer = (char *)malloc(BUFFER_SIZE);
    if (!buffer) {
        log_trace("assemble_multiple_cids: Memory allocation failed");
        exit(-1);
    }

    for (int j = 0; j < info->num_cids; j++) {
        append_cid_output(info->filename, info->cids[j], outfile, buffer,
                          config);
    }

    fclose(outfile);
    free(buffer);
}

static void log_assembly(file_info_t *info) {
    if (!info) {
        log_trace("log_assembly: NULL info");
        return;
    }

    log_trace("assemble: track: %d / %d", info->track_id,
              info->config->num_tracks);
    log_trace("assemble: path: %s",
              info->album_path ? info->album_path : "NULL");
    log_trace("assemble: filename: %s",
              info->track_name ? info->track_name : "NULL");

    if (info->num_cids == 1) {
        log_trace("assemble: info: %s -> %s",
                  info->cids[0] ? info->cids[0] : "NULL",
                  info->filename ? info->filename : "NULL");
    } else {
        log_trace("assemble: info: %d CIDs -> %s", info->num_cids,
                  info->filename ? info->filename : "NULL");
    }
}

static void move_single_file(file_info_t *info, char *file_path,
                             config_t *config) {
    if (!info || !file_path || !config || !info->cids[0]) {
        log_trace("move_single_file: Invalid parameters");
        exit(-1);
    }

    char *cid_path = util_make_path(config->output, info->cids[0]);
    if (!cid_path) {
        log_trace("move_single_file: Failed to get CID path");
        exit(-1);
    }

    if (rename(cid_path, file_path) != 0) {
        log_trace("move_single_file: Failed to move file %s to %s",
                  info->cids[0], info->filename);
        exit(-1);
    }

    log_trace("move_single_file: %s -> %s", info->cids[0], info->filename);
    free(cid_path);
}

static void assemble_file(file_info_t *info, config_t *config) {
    if (!info || !config) {
        log_trace("assemble_file: Invalid parameters");
        exit(-1);
    }

    log_trace("assemble_file: start assembling %s",
              info->filename ? info->filename : "NULL");
    log_assembly(info);

    char *file_path = util_make_path(config->output, info->filename);
    if (!file_path) {
        log_trace("assemble_file: Failed to get file path");
        exit(-1);
    }

    if (info->num_cids == 1) {
        move_single_file(info, file_path, config);
    } else {
        assemble_multiple_cids(info, file_path, config);
    }

    free(file_path);
    log_trace("assemble: finish assembling %s",
              info->filename ? info->filename : "NULL");
}
