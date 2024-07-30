#ifndef CONFIG_H
#define CONFIG_H

#include <jansson.h>

typedef struct {
    char *db;
    char *output;
    int max_retries;
    int timeout;
    char **gateways;
    int num_gateways;
    int num_tracks;
    int num_files;
    char *pipe_name;
} config_t;

void read_config(const char *config_file, config_t *config);
void free_config(config_t *config);

#endif // CONFIG_H
