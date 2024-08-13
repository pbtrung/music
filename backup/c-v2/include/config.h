#ifndef CONFIG_H
#define CONFIG_H

#include <apr_pools.h>

typedef struct {
    char *db;
    char *output;
    int max_retries;
    int timeout;
    char **gateways;
    int num_gateways;
    int num_tracks;
    int min_value;
    int num_files;
    char *pipe_name;
    char *log;
    char *ffmpeg_log;
} config_t;

void config_read(const char *config_file, config_t *config);
apr_status_t config_free(void *data);

#endif // CONFIG_H
