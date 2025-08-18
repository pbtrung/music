#include <stdio.h>

#include <apr_strings.h>

#include "cosmosdb.h"
#include "download.h"

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

    json_decref(doc);
    apr_pool_destroy(pool);
    apr_terminate();
    return 0;
}
