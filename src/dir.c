#include <stdlib.h>

#include "dir.h"
#include "log.h"
#include "utils.h"

int dir_delete(apr_pool_t *pool, const char *path) {
    apr_status_t rv;
    apr_dir_t *dir;
    apr_finfo_t finfo;
    char *filepath;

    // Check if path exists and is a directory
    rv = apr_stat(&finfo, path, APR_FINFO_TYPE, pool);
    if (rv != APR_SUCCESS || finfo.filetype != APR_DIR) {
        log_trace("dir_delete: Invalid path or not a directory: %s", path);
        util_error_exit("dir_delete: Invalid path or not a directory");
    }

    // Open the directory
    rv = apr_dir_open(&dir, path, pool);
    if (rv != APR_SUCCESS) {
        log_trace("dir_delete: Failed to open directory: %s", path);
        util_error_exit("dir_delete: Failed to open directory");
    }

    // Iterate over directory entries
    while (apr_dir_read(&finfo, APR_FINFO_DIRENT | APR_FINFO_TYPE, dir) ==
           APR_SUCCESS) {
        if (strcmp(finfo.name, ".") == 0 || strcmp(finfo.name, "..") == 0)
            continue; // Skip "." and ".."

        filepath = apr_pstrcat(pool, path, "/", finfo.name, NULL);

        if (finfo.filetype == APR_DIR) {
            // Recursively delete subdirectory
            if (dir_delete(pool, filepath) != 0) {
                log_trace("dir_delete: Failed to delete subdirectory: %s",
                          filepath);
                apr_dir_close(dir);
                exit(-1);
            }
        } else {
            // Delete file
            rv = apr_file_remove(filepath, pool);
            if (rv != APR_SUCCESS) {
                log_trace("dir_delete: Failed to delete file: %s", filepath);
                util_error_exit("dir_delete: Failed to delete file");
            }
        }
    }

    apr_dir_close(dir);

    // Delete the directory itself
    rv = apr_dir_remove(path, pool);
    if (rv != APR_SUCCESS) {
        log_trace("dir_delete: Failed to remove directory: %s", path);
        util_error_exit("dir_delete: Failed to remove directory");
    }

    return 0;
}

void dir_create(apr_pool_t *pool, const char *path) {
    apr_status_t rv;

    rv = apr_dir_make(path, APR_UREAD | APR_UWRITE | APR_UEXECUTE, pool);
    if (rv != APR_SUCCESS) {
        log_trace("dir_create: Failed to create directory: %s", path);
        util_error_exit("dir_create: Failed to create directory");
    }
}
