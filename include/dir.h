#ifndef DIR_H
#define DIR_H

#include <apr_file_io.h>
#include <apr_pools.h>
#include <apr_strings.h>

int dir_delete(apr_pool_t *pool, const char *path);
void dir_create(apr_pool_t *pool, const char *path);

#endif // DIR_H