#include "config.h"
#include "log.h"
#include <jansson.h>
#include <string.h>

apr_status_t config_free(void *data) {
    log_trace("config_free: start");
    config_t *config = (config_t *)data;
    free(config->db);
    free(config->output);
    free(config->log);
    free(config->pipe_name);
    for (int i = 0; i < config->num_gateways; ++i) {
        free(config->gateways[i]);
    }
    free(config->gateways);
    log_trace("config_free: finish");
    return APR_SUCCESS;
}

void config_read(const char *config_file, config_t *config) {
    json_error_t error;
    json_t *root = json_load_file(config_file, 0, &error);

    if (!root) {
        log_trace("config_read: Failed to parse JSON config: %s", error.text);
        exit(-1);
    }

    json_t *db_obj = json_object_get(root, "db");
    json_t *output_obj = json_object_get(root, "output");
    json_t *max_retries_obj = json_object_get(root, "max_retries");
    json_t *timeout_obj = json_object_get(root, "timeout");
    json_t *gateways_array = json_object_get(root, "gateways");
    json_t *num_files_obj = json_object_get(root, "num_files");
    json_t *min_value_obj = json_object_get(root, "min_value");
    json_t *pipe_name_obj = json_object_get(root, "pipe_name");
    json_t *log_obj = json_object_get(root, "log");

    if (!json_is_string(db_obj) || !json_is_string(output_obj) ||
        !json_is_integer(max_retries_obj) || !json_is_integer(timeout_obj) ||
        !json_is_array(gateways_array) || !json_is_integer(num_files_obj) ||
        !json_is_string(pipe_name_obj) || !json_is_integer(min_value_obj) ||
        !json_is_string(log_obj)) {

        log_trace("config_read: Invalid config file format");
        exit(-1);
    }

    config->db = strdup(json_string_value(db_obj));
    config->output = strdup(json_string_value(output_obj));
    config->max_retries = json_integer_value(max_retries_obj);
    config->timeout = json_integer_value(timeout_obj);
    config->num_files = json_integer_value(num_files_obj);
    config->pipe_name = strdup(json_string_value(pipe_name_obj));
    config->log = strdup(json_string_value(log_obj));
    config->min_value = json_integer_value(min_value_obj);

    config->num_gateways = json_array_size(gateways_array);
    config->gateways = malloc(config->num_gateways * sizeof(char *));

    for (size_t i = 0; i < config->num_gateways; i++) {
        json_t *gateway_obj = json_array_get(gateways_array, i);

        if (!json_is_string(gateway_obj)) {
            log_trace("config_read: Invalid gateway format");
            exit(-1);
        }

        config->gateways[i] = strdup(json_string_value(gateway_obj));
    }

    json_decref(root);
}
