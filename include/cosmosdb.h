#ifndef COSMOSDB_H
#define COSMOSDB_H

#include <apr_pools.h>
#include <jansson.h>

/* Cosmos DB account + key (SQL API) */
typedef struct {
    const char *account;    /* e.g., "myaccount" (no .documents.azure.com) */
    const char *master_key; /* base64-encoded primary/secondary key */
} cosmosdb_credentials_t;

/* Init/cleanup APR + cURL globals and a root pool */
int cosmosdb_init(apr_pool_t **pool);
void cosmosdb_cleanup(apr_pool_t *pool);

/*
 * Read-only: GET a single document.
 *   resource_link: "dbs/{dbId}/colls/{containerId}/docs/{docId}"
 *   resource_type: "docs"
 *   partition_key: optional, e.g., "1" -> header x-ms-documentdb-partitionkey:
 * ["1"]
 *
 * Returns parsed JSON (caller must json_decref), or NULL on error.
 */
json_t *cosmosdb_get_item(apr_pool_t *pool, const cosmosdb_credentials_t *creds,
                          const char *resource_link, const char *resource_type,
                          const char *partition_key);

#endif /* COSMOSDB_H */
