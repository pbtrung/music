#include "download.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <stdexcept>

typedef struct {
    char *cid;
    cid_info_t *cid_info;
    config_t *config;
} download_info_t;

void download_cleanup(file_info_t *infos, int num_files) {
    for (int i = 0; i < num_files; i++) {
        file_info_t *info = &infos[i];

        free(info->cid_infos);
        free(info->filename);
        free(info->cids);
        free(info->track_name);
        free(info->album_path);
        free(info->ext);
    }
}

void download_init(file_info_t *infos, config_t *config, sqlite3 *db) {
    int *random_index = random_ints(config->num_files, 1, config->num_tracks);

    int num_cids;
    for (int i = 0; i < config->num_files; ++i) {
        infos[i].track_name = get_track_name(db, random_index[i]);
        infos[i].album_path = get_album(db, random_index[i]);
        infos[i].filename = get_filename_with_ext(infos[i].track_name);
        infos[i].ext = get_extension(infos[i].track_name);
        infos[i].cids = get_cids(db, random_index[i], &num_cids);
        infos[i].num_cids = num_cids;
        infos[i].config = config;
        infos[i].download_status = DOWNLOAD_PENDING;

        infos[i].cid_infos =
            (cid_info_t *)malloc(infos[i].num_cids * sizeof(cid_info_t));
        if (!infos[i].cid_infos) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        for (int j = 0; j < infos[i].num_cids; ++j) {
            infos[i].cid_infos[j].download_status = DOWNLOAD_PENDING;
        }
    }

    free(random_index);
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static void download_cid(uv_work_t *req) {
    download_info_t *download_info = (download_info_t *)req->data;

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
        get_file_path(download_info->config->output, download_info->cid);

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
                random_ints(1, 0, download_info->config->num_gateways - 1);
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
                download_info->cid_info->download_status = DOWNLOAD_OK;
                break;
            }
        }
        retries++;
        rewind(fp);
    } while (retries < download_info->config->max_retries);

    if (res != CURLE_OK || response_code != 200) {
        fprintf(stderr, "Download of cid %s failed after %d tries\n",
                download_info->cid, retries);
        download_info->cid_info->download_status = DOWNLOAD_FAILED;
    }

    fclose(fp);
    free(file_path);
    curl_easy_cleanup(curl);
}

static void on_cid_download_completed(uv_work_t *req, int status) {
    download_info_t *download_info = (download_info_t *)req->data;
    free(download_info);
    free(req);
}

static void assemble(file_info_t *info, config_t *config) {
    char *file_path = get_file_path(config->output, info->filename);

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
        char *cid_path = get_file_path(config->output, info->cids[j]);

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
        infos[i].download_status = DOWNLOAD_OK;
        for (int j = 0; j < infos[i].num_cids; ++j) {
            if (infos[i].cid_infos[j].download_status != DOWNLOAD_OK) {
                infos[i].download_status = DOWNLOAD_FAILED;
            }
        }
    }
    const int width = 20;
    fprintf(stdout, "\n");
    for (int i = 0; i < config->num_files; ++i) {
        if (infos[i].download_status == DOWNLOAD_OK) {
            fprintf(stdout, "%-*s: %s\n", width + 2, "Assemble",
                    infos[i].filename);
            fprintf(stdout, "  %-*s: %s\n", width, "path", infos[i].album_path);
            fprintf(stdout, "  %-*s: %s\n", width, "filename",
                    infos[i].track_name);
            fprintf(stdout, "\n");
            fflush(stdout);
            assemble(&infos[i], config);
        }
    }
}

void download_files(file_info_t *infos, config_t *config) {
    uv_loop_t *loop = uv_default_loop();
    fprintf(stdout, "\n");
    for (int i = 0; i < config->num_files; ++i) {
        for (int j = 0; j < infos[i].num_cids; ++j) {
            uv_work_t *req = malloc(sizeof(uv_work_t));
            if (!req) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(-1);
            }
            download_info_t *download_info = malloc(sizeof(download_info_t));
            if (!download_info) {
                fprintf(stderr, "Memory allocation failed\n");
                exit(-1);
            }
            download_info->cid = infos[i].cids[j];
            download_info->cid_info = &infos[i].cid_infos[j];
            download_info->config = config;
            req->data = download_info;

            uv_queue_work(loop, req, download_cid, on_cid_download_completed);
        }
    }
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
}