#include <stdio.h>

#include "cosmosdb.h"

int main() {
    apr_pool_t *pool;
    if (cosmosdb_init(&pool) != 0) {
        fprintf(stderr, "Init failed\n");
        return 1;
    }

    cosmosdb_credentials_t creds = {
        .account = "myaccount",
        .master_key = "BASE64ENCODEDPRIMARYKEY=="
    };

    json_t *doc = cosmosdb_get_item(pool, &creds,
                                    "dbs/MyDB/colls/MyColl/docs/1",
                                    "docs",
                                    "1");  // partition key = "1"

    if (doc) {
        char *dump = json_dumps(doc, JSON_INDENT(2));
        printf("%s\n", dump);
        free(dump);
        json_decref(doc);
    }

    cosmosdb_cleanup(pool);
    return 0;
}
