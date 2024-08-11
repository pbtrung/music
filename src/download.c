#include "download.h"
#include "const.h"
#include <apr_thread_pool.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

apr_status_t download_cleanup(void *data) {
    file_infos_t *file_infos_cleaner = (file_infos_t *)data;
    file_info_t *file_infos = file_infos_cleaner->file_infos;
    for (int i = 0; i < file_infos_cleaner->num_files; i++) {
        file_info_t *info = &file_infos[i];
        free(info->track_name);
        free(info->album_path);
        free(info->filename);
        free(info->extension);
        free(info->cids);
        free(info->cid_download_status);
    }
    return APR_SUCCESS;
}

void download_init(file_info_t *infos, config_t *config, sqlite3 *db) {
    int *random_index = util_random_ints(config->num_files, config->min_value,
                                         config->num_tracks);

    int num_cids;
    for (int i = 0; i < config->num_files; ++i) {
        infos[i].track_name = database_get_track_name(db, random_index[i]);
        infos[i].album_path = database_get_album(db, random_index[i]);
        infos[i].filename =
            util_get_filename_with_extension(infos[i].track_name);
        infos[i].extension = util_get_extension(infos[i].track_name);
        infos[i].cids = database_get_cids(db, random_index[i], &num_cids);
        infos[i].num_cids = num_cids;
        infos[i].config = config;
        infos[i].file_download_status = DOWNLOAD_PENDING;

        infos[i].cid_download_status = (enum download_status *)malloc(
            num_cids * sizeof(enum download_status));
        if (!infos[i].cid_download_status) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        for (int j = 0; j < infos[i].num_cids; ++j) {
            infos[i].cid_download_status[j] = DOWNLOAD_PENDING;
        }
    }

    free(random_index);
}

typedef struct {
    char *cid;
    enum download_status *cid_download_status;
    config_t *config;
} download_info_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static void *APR_THREAD_FUNC download_cid(apr_thread_t *thd, void *data) {
    download_info_t *download_info = (download_info_t *)data;

    fprintf(stdout, "Downloading %s\n", download_info->cid);
    fflush(stdout);

    int retries = 0;
    CURLcode res;
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error initializing curl handle\n");
        exit(-1);
    }
    char *file_path =
        util_get_file_path(download_info->config->output, download_info->cid);

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open file %s\n", file_path);
        exit(-1);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    long response_code;
    char url[128];
    do {
        if (strlen(download_info->cid) == 59) {
            snprintf(url, 128, "https://%s.ipfs.nftstorage.link",
                     download_info->cid);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                             2 * download_info->config->timeout);
        } else {
            int *random_index =
                util_random_ints(1, 0, download_info->config->num_gateways - 1);
            snprintf(url, 128, "https://%s/%s",
                     download_info->config->gateways[*random_index],
                     download_info->cid);
            free(random_index);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                             download_info->config->timeout);
        }
        curl_easy_setopt(curl, CURLOPT_URL, url);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code == 200) {
                *(download_info->cid_download_status) = DOWNLOAD_SUCCEEDED;
                break;
            }
        }
        retries++;
        rewind(fp);
    } while (retries < download_info->config->max_retries);

    if (res != CURLE_OK || response_code != 200) {
        fprintf(stderr, "Download of cid %s failed after %d tries\n",
                download_info->cid, retries + 1);
        *(download_info->cid_download_status) = DOWNLOAD_FAILED;
    }

    fclose(fp);
    free(file_path);
    curl_easy_cleanup(curl);
    return NULL;
}

void download_files(apr_pool_t *pool, file_info_t *infos, config_t *config) {
    apr_thread_pool_t *thread_pool;
    apr_status_t status;
    apr_pool_t *subpool;
    apr_pool_create(&subpool, pool);

    status = apr_thread_pool_create(&thread_pool, 10, 20, subpool);
    if (status != APR_SUCCESS) {
        fprintf(stderr, "Error creating thread pool\n");
        exit(-1);
    }

    for (int i = 0; i < config->num_files; ++i) {
        for (int j = 0; j < infos[i].num_cids; ++j) {
            download_info_t *download_info =
                apr_palloc(subpool, sizeof(download_info_t));
            download_info->cid = infos[i].cids[j];
            download_info->cid_download_status =
                &(infos[i].cid_download_status[j]);
            download_info->config = config;
            status = apr_thread_pool_push(thread_pool, download_cid,
                                          download_info, 0, NULL);
            if (status != APR_SUCCESS) {
                fprintf(stderr,
                        "Failed to push task to thread pool for cid %s\n",
                        infos[i].cids[j]);
                exit(-1);
            }
        }
    }

    while (apr_thread_pool_tasks_count(thread_pool) > 0) {
        apr_sleep(apr_time_from_sec(1));
    }

    apr_thread_pool_destroy(thread_pool);
    apr_pool_destroy(subpool);
}

static void assemble(file_info_t *info, config_t *config) {
    char *file_path = util_get_file_path(config->output, info->filename);

    FILE *outfile = fopen(file_path, "wb");
    if (!outfile) {
        fprintf(stderr, "Failed to open file %s\n", file_path);
        exit(-1);
    }

    size_t buffer_size = 4096;
    char *buffer = (char *)malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    size_t bytes_read;
    for (int j = 0; j < info->num_cids; j++) {
        char *cid_path = util_get_file_path(config->output, info->cids[j]);

        FILE *infile = fopen(cid_path, "rb");
        if (!infile) {
            fprintf(stderr, "Failed to open file %s\n", cid_path);
            exit(-1);
        }

        while ((bytes_read = fread(buffer, 1, buffer_size, infile)) > 0) {
            fwrite(buffer, 1, bytes_read, outfile);
        }
        fclose(infile);

        if (remove(cid_path) != 0) {
            fprintf(stderr, "Failed to delete file %s\n", cid_path);
            exit(-1);
        }
        free(cid_path);
    }

    fclose(outfile);
    free(file_path);
    free(buffer);
}

void assemble_files(file_info_t *infos, config_t *config) {
    for (int i = 0; i < config->num_files; ++i) {
        infos[i].file_download_status = DOWNLOAD_SUCCEEDED;
        for (int j = 0; j < infos[i].num_cids; ++j) {
            if (infos[i].cid_download_status[j] != DOWNLOAD_SUCCEEDED) {
                infos[i].file_download_status = DOWNLOAD_FAILED;
            }
        }
    }
    fprintf(stdout, "\n");
    for (int i = 0; i < config->num_files; ++i) {
        if (infos[i].file_download_status == DOWNLOAD_SUCCEEDED) {
            fprintf(stdout, "%-*s: %s\n", WIDTH + 2, "Assemble",
                    infos[i].filename);
            fprintf(stdout, "  %-*s: %s\n", WIDTH, "path", infos[i].album_path);
            fprintf(stdout, "  %-*s: %s\n", WIDTH, "filename",
                    infos[i].track_name);
            fprintf(stdout, "\n");
            fflush(stdout);
            assemble(&infos[i], config);
        }
    }
}