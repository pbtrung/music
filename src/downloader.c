#include "downloader.h"
#include "dir.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <ctype.h>
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void to_lowercase(char *str) {
    for (; *str; ++str)
        *str = tolower((unsigned char)*str);
}

char *get_extension(const char *text) {
    pcre2_code *re;
    pcre2_match_data *match_data;
    int errcode;
    PCRE2_SIZE erroffset;
    PCRE2_SIZE *ovector;
    size_t ext_len;
    char *ext = NULL;

    // Compile the regular expression for file extension
    re = pcre2_compile(
        (PCRE2_SPTR) "(.*)\\.(opus|mp3|m4a)$",
        PCRE2_ZERO_TERMINATED, // Pattern length (0 means null-terminated)
        PCRE2_CASELESS,        // Options
        &errcode,              // Error code
        &erroffset,            // Error offset
        NULL);                 // Compile context

    if (!re) {
        fprintf(stderr, "PCRE2 compilation failed\n");
        exit(-1);
    }

    // Create a match data block
    match_data = pcre2_match_data_create_from_pattern(re, NULL);
    if (!match_data) {
        fprintf(stderr, "Failed to create match data\n");
        exit(-1);
    }

    // Perform the match
    int rc = pcre2_match(re,               // Compiled pattern
                         (PCRE2_SPTR)text, // Subject string
                         strlen(text),     // Length of subject string
                         0,                // Starting offset
                         0,                // Options
                         match_data,       // Match data
                         NULL              // Match context
    );

    if (rc < 0) {
        fprintf(stderr, "No match\n");
        exit(-1);
    }

    // Extract the file extension from the match data
    ovector = pcre2_get_ovector_pointer(match_data);

    if (rc >= 3) {
        // Group 2 contains the file extension
        ext_len = ovector[5] - ovector[4];
        ext = (char *)malloc(ext_len + 1);
        if (!ext) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        memcpy(ext, text + ovector[4], ext_len);
        ext[ext_len] = '\0'; // Null-terminate the string
    }

    to_lowercase(ext);
    pcre2_code_free(re);
    pcre2_match_data_free(match_data);

    return ext;
}

void on_file_download_complete(uv_work_t *req, int status) {}

void on_cid_download_complete(uv_work_t *req, int status) {
    cid_downloader_t *task = (cid_downloader_t *)req->data;

    int num_downloaded = 0;
    for (int j = 0; j < task->num_cids; j++) {
        num_downloaded += task->downloaded[j];
    }
    if (num_downloaded == task->num_cids) {
        int path_length =
            strlen(task->config->output) + strlen(task->cids[0]) * 2;
        char *path = (char *)malloc(path_length);
        if (!path) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        snprintf(path, path_length, "%s/%s", task->config->output,
                 task->filename);

        FILE *outfile = fopen(path, "wb");
        if (!outfile) {
            fprintf(stderr, "Failed to open file %s\n", path);
            exit(-1);
        }
        char buffer[4096];
        size_t bytes_read;
        for (int j = 0; j < task->num_cids; j++) {
            snprintf(path, path_length, "%s/%s", task->config->output,
                     task->cids[j]);

            FILE *infile = fopen(path, "rb");
            if (!infile) {
                fprintf(stderr, "Failed to open file %s\n", path);
                exit(-1);
            }

            while ((bytes_read = fread(buffer, 1, sizeof(buffer), infile)) >
                   0) {
                fwrite(buffer, 1, bytes_read, outfile);
            }
            fclose(infile);

            if (remove(path) != 0) {
                fprintf(stderr, "Failed to delete file %s\n", path);
                exit(-1);
            }
        }
        fclose(outfile);
        free(path);
    }
}

static size_t write_callback(void *ptr, size_t size, size_t nmemb,
                             FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

void download_cid_task(uv_work_t *req) {
    cid_downloader_t *task = (cid_downloader_t *)req->data;

    CURLcode res;

    task->curl = curl_easy_init();
    if (!task->curl) {
        fprintf(stderr, "Error initializing curl handle\n");
        exit(-1);
    }

    int path_length =
        strlen(task->config->output) + strlen(task->cids[task->cid_index]) + 1;
    char *path = (char *)malloc(path_length + 1);
    if (!path) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    snprintf(path, path_length + 1, "%s/%s", task->config->output,
             task->cids[task->cid_index]);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open file %s\n", path);
        exit(-1);
    }

    curl_easy_setopt(task->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(task->curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(task->curl, CURLOPT_FOLLOWLOCATION, 1L);

    char url[256];
    do {
        if (strlen(task->cids[task->cid_index]) == 59) {
            snprintf(url, 256, "https://%s.ipfs.nftstorage.link",
                     task->cids[task->cid_index]);
            curl_easy_setopt(task->curl, CURLOPT_TIMEOUT, 50);
        } else {
            int *random_index =
                random_ints(1, 0, task->config->num_gateways - 1);
            snprintf(url, 256, "https://%s/%s",
                     task->config->gateways[*random_index],
                     task->cids[task->cid_index]);
            free(random_index);
            curl_easy_setopt(task->curl, CURLOPT_TIMEOUT,
                             task->config->timeout);
        }
        curl_easy_setopt(task->curl, CURLOPT_URL, url);

        res = curl_easy_perform(task->curl);
        if (res == CURLE_OK) {
            task->downloaded[task->cid_index] = 1;
            break;
        } else {
            fprintf(stderr, "Error downloading cid %s of %s (attempt %d)\n",
                    task->cids[task->cid_index], task->filename,
                    task->retries + 1);
            task->retries++;
            rewind(fp);
        }
    } while (task->retries < task->config->max_retries - 1);

    if (res != CURLE_OK) {
        fprintf(stderr, "Download of cid %s of %s failed after %d tries\n",
                task->cids[task->cid_index], task->filename, task->retries);
    }

    task->completed[task->cid_index] = 1;
    fclose(fp);
    free(path);
    curl_easy_cleanup(task->curl);
}

void download_file_task(uv_work_t *req) {
    file_downloader_t *info = (file_downloader_t *)req->data;

    for (int i = 0; i < info->num_cids; i++) {
        info->cid_tasks[i].work_req.data = &info->cid_tasks[i];
        uv_queue_work(info->loop, &info->cid_tasks[i].work_req,
                      download_cid_task, on_cid_download_complete);
    }
}

void perform_downloads(file_downloader_t *infos, int num_files,
                       uv_loop_t *loop) {
    printf("start downloads\n");
    for (int i = 0; i < num_files; i++) {
        infos[i].loop = loop;
        infos[i].work_req.data = &infos[i];
        uv_queue_work(infos[i].loop, &infos[i].work_req, download_file_task,
                      on_file_download_complete);
    }
}

char *get_filename_with_ext(char *track_name) {
    char *ext = get_extension(track_name);
    int fn_len = 20;
    char *fn = random_string(fn_len);
    int filename_len = fn_len + 1 + strlen(ext);
    char *filename = (char *)malloc(filename_len + 1);
    if (!filename) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    snprintf(filename, filename_len + 1, "%s.%s", fn, ext);
    free(fn);
    free(ext);
    return filename;
}

void initialize_downloads(file_downloader_t *infos, int num_files,
                          config_t *config, sqlite3 *db) {
    int min_value = 1;
    int max_value = config->num_tracks;
    int *random_index = random_ints(num_files, min_value, max_value);

    int num_cids;
    for (int i = 0; i < num_files; ++i) {
        infos[i].track_name = get_track_name(db, random_index[i]);
        infos[i].album_path = get_album(db, random_index[i]);
        infos[i].filename = get_filename_with_ext(infos[i].track_name);
        infos[i].ext = get_extension(infos[i].track_name);
        infos[i].cids = get_cids(db, random_index[i], &num_cids);
        infos[i].num_cids = num_cids;
        infos[i].config = config;
        infos[i].downloaded = calloc(infos[i].num_cids, sizeof(int));
        infos[i].completed = calloc(infos[i].num_cids, sizeof(int));

        infos[i].cid_tasks = (cid_downloader_t *)malloc(
            infos[i].num_cids * sizeof(cid_downloader_t));
        if (!infos[i].cid_tasks) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        for (int j = 0; j < infos[i].num_cids; j++) {
            infos[i].cid_tasks[j].config = config;
            infos[i].cid_tasks[j].cids = infos[i].cids;
            infos[i].cid_tasks[j].retries = 0;
            infos[i].cid_tasks[j].cid_index = j;
            infos[i].cid_tasks[j].downloaded = infos[i].downloaded;
            infos[i].cid_tasks[j].completed = infos[i].completed;
            infos[i].cid_tasks[j].num_cids = infos[i].num_cids;
            infos[i].cid_tasks[j].filename = infos[i].filename;
        }
    }

    free(random_index);
    return;
}

void cleanup_downloads(file_downloader_t *infos, int num_files) {
    for (int i = 0; i < num_files; i++) {
        file_downloader_t *info = &infos[i];

        // for (int j = 0; j < info->num_cids; j++) {
        //     cid_downloader_t *task = &info->cid_tasks[j];
        //     if (task->curl != NULL) {
        //         curl_easy_cleanup(task->curl);
        //     }
        // }

        free(info->cid_tasks);
        free(info->filename);
        free(info->cids);
        free(info->downloaded);
        free(info->track_name);
        free(info->album_path);
        free(info->completed);
        free(info->ext);
    }
}
