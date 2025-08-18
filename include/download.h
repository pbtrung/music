#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <apr_pools.h>

#include "config.h"
#include "file_info.h"
#include "utils.h"

void download_assemble_file(apr_pool_t *pool, config_t *config,
                            file_info_t *info);

#endif // DOWNLOAD_H