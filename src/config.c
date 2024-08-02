#include "config.h"
#include <jansson.h>
#include <string.h>

void free_config(config_t *config) {
    free(config->db);
    free(config->output);
    free(config->pipe_name);
    free(config->ws_pipeout);
    free(config->ws_pipein);
    for (int i = 0; i < config->num_gateways; ++i) {
        free(config->gateways[i]);
    }
    free(config->gateways);
}

void read_config(const char *config_file, config_t *config) {
    json_error_t error;
    json_t *root = json_load_file(config_file, 0, &error);

    if (!root) {
        fprintf(stderr, "Failed to parse JSON config: %s\n", error.text);
        exit(-1);
    }

    json_t *db_obj = json_object_get(root, "db");
    json_t *output_obj = json_object_get(root, "output");
    json_t *max_retries_obj = json_object_get(root, "max_retries");
    json_t *timeout_obj = json_object_get(root, "timeout");
    json_t *gateways_array = json_object_get(root, "gateways");
    json_t *num_files_obj = json_object_get(root, "num_files");
    json_t *pipe_name_obj = json_object_get(root, "pipe_name");
    json_t *ws_pipeout_obj = json_object_get(root, "ws_pipeout");
    json_t *ws_pipein_obj = json_object_get(root, "ws_pipein");

    if (!json_is_string(db_obj) || !json_is_string(output_obj) ||
        !json_is_integer(max_retries_obj) || !json_is_integer(timeout_obj) ||
        !json_is_array(gateways_array) || !json_is_integer(num_files_obj) ||
        !json_is_string(pipe_name_obj)) {

        fprintf(stderr, "Invalid config file format\n");
        exit(-1);
    }

    config->db = strdup(json_string_value(db_obj));
    config->output = strdup(json_string_value(output_obj));
    config->max_retries = json_integer_value(max_retries_obj);
    config->timeout = json_integer_value(timeout_obj);
    config->num_files = json_integer_value(num_files_obj);
    config->pipe_name = strdup(json_string_value(pipe_name_obj));
    config->ws_pipeout = strdup(json_string_value(ws_pipeout_obj));
    config->ws_pipein = strdup(json_string_value(ws_pipein_obj));

    config->num_gateways = json_array_size(gateways_array);
    config->gateways = malloc(config->num_gateways * sizeof(char *));

    for (size_t i = 0; i < config->num_gateways; i++) {
        json_t *gateway_obj = json_array_get(gateways_array, i);

        if (!json_is_string(gateway_obj)) {
            fprintf(stderr, "Invalid gateway format\n");
            exit(-1);
        }

        config->gateways[i] = strdup(json_string_value(gateway_obj));
    }

    json_decref(root);
}
