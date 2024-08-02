#ifndef CONFIG_H
#define CONFIG_H

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
    char *ws_pipeout;
    char *ws_pipein;
} config_t;

void read_config(const char *config_file, config_t *config);
void free_config(config_t *config);

#endif // CONFIG_H
