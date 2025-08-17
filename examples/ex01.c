#include <stdio.h>

#include <apr_strings.h>

#include "cosmosdb.h"
#include "download.h"

static void print_file_info(const file_info_t *info) {
    if (!info) {
        printf("(null file_info_t)\n");
        return;
    }

    printf("=== file_info_t ===\n");
    printf("Filename: %s\n", info->filename ? info->filename : "(null)");
    printf("Track Name: %s\n", info->track_name ? info->track_name : "(null)");
    printf("Extension: %s\n", info->extension ? info->extension : "(null)");
    printf("Album Path: %s\n", info->album_path ? info->album_path : "(null)");
    printf("Track ID: %d\n", info->track_id);

    printf("Number of CIDs: %d\n", info->num_cids);
    if (info->cids) {
        for (int i = 0; i < info->num_cids; i++) {
            printf("  CID[%d]: %s\n", i, info->cids[i] ? info->cids[i] : "(null)");
            if (info->cid_download_status) {
                printf("    CID Status: %d\n", info->cid_download_status[i]);
            }
        }
    }

    printf("File Download Status: %d\n", info->file_download_status);
    printf("Config pointer: %p\n", (void *)info->config);
}

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

int main(int argc, const char *argv[]) {
    if (argc != 2)
        exit_on_error("Usage: <program> <config_file>");

    apr_pool_t *pool;
    initialize_pool(&pool);    

    config_t *cfg = load_config(argv[1]);
    json_t *doc = cosmosdb_get_item(pool, cfg);
    config_free(cfg);
    free(cfg);
    cfg = NULL;

    if (doc) {
        char *dump = json_dumps(doc, JSON_INDENT(2));
        printf("%s\n", dump);
        free(dump);
    }

    file_info_t *info = apr_palloc(pool, sizeof(file_info_t));
    apr_pool_cleanup_register(pool, info, file_info_free,
                              apr_pool_cleanup_null);
    cosmosdb_file_info_init(info, doc, cfg);
    // print_file_info(info);

    json_decref(doc);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}
