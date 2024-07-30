#include "dir.h"

int delete_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        char filepath[1024];

        if (!dir) {
            fprintf(stderr, "opendir\n");
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;

            snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);

            if (entry->d_type == DT_DIR) {
                // Recursively delete subdirectory
                if (delete_directory(filepath) != 0) {
                    fprintf(stderr, "delete_directory\n");
                    return -1;
                }
            } else {
                // Delete file
                if (unlink(filepath) != 0) {
                    fprintf(stderr, "unlink\n");
                    return -1;
                }
            }
        }

        closedir(dir);

        // Delete the directory itself
        if (rmdir(path) != 0) {
            fprintf(stderr, "rmdir\n");
            return -1;
        }
    }
    return 0;
}

// Function to create a directory
void create_directory(const char *path) {
    if (mkdir(path, 0755) != 0) {
        fprintf(stderr, "mkdir\n");
        exit(-1);
    }
}
