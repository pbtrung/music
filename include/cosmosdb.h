#ifndef COSMOSDB_H
#define COSMOSDB_H

#include <apr_pools.h>
#include <jansson.h>

#include "config.h"
#include "download.h"

json_t *cosmosdb_get_item(apr_pool_t *pool, const config_t *config);
void cosmosdb_file_info_init(file_info_t *info, json_t *doc, config_t *config);

#endif // COSMOSDB_H
