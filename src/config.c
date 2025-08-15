#include <string.h>

#include <jansson.h>

#include "config.h"
#include "log.h"

apr_status_t config_free(void *data) {
    log_trace("config_free: start");

    config_t *config = (config_t *)data;

    free(config->db);
    free(config->output);
    free(config->log);
    free(config->pipe_name);
    free(config->n_gateway);
    free(config->i_gateway);
    free(config->cosmos_uri);
    free(config->cosmos_key);
    free(config->cosmos_db_name);
    free(config->cosmos_container);

    for (int i = 0; i < config->num_gateways; ++i) {
        free(config->gateways[i]);
    }
    free(config->gateways);

    log_trace("config_free: finish");
    return APR_SUCCESS;
}

static char *safe_string_duplicate(const char *src) {
    if (!src) {
        log_trace("safe_string_duplicate: null source string");
        return NULL;
    }

    char *copy = strdup(src);
    if (!copy) {
        log_trace("safe_string_duplicate: failed to duplicate string");
    }

    return copy;
}

static bool validate_json_fields(json_t *root) {
    json_t *db_obj = json_object_get(root, "db");
    json_t *output_obj = json_object_get(root, "output");
    json_t *max_retries_obj = json_object_get(root, "max_retries");
    json_t *timeout_obj = json_object_get(root, "timeout");
    json_t *gateways_array = json_object_get(root, "gateways");
    json_t *num_files_obj = json_object_get(root, "num_files");
    json_t *min_value_obj = json_object_get(root, "min_value");
    json_t *pipe_name_obj = json_object_get(root, "pipe_name");
    json_t *log_obj = json_object_get(root, "log");
    json_t *n_gateway_obj = json_object_get(root, "n_gateway");
    json_t *i_gateway_obj = json_object_get(root, "i_gateway");
    json_t *ncores_obj = json_object_get(root, "ncores");
    json_t *mul_factor_obj = json_object_get(root, "mul_factor");
    json_t *cosmos_uri_obj = json_object_get(root, "cosmos_uri");
    json_t *cosmos_key_obj = json_object_get(root, "cosmos_key");
    json_t *cosmos_db_name_obj = json_object_get(root, "cosmos_db_name");
    json_t *cosmos_container_obj = json_object_get(root, "cosmos_container");
    json_t *max_value_obj = json_object_get(root, "max_value");

    if (!json_is_string(db_obj) || !json_is_string(output_obj) ||
        !json_is_integer(max_retries_obj) || !json_is_integer(timeout_obj) ||
        !json_is_array(gateways_array) || !json_is_integer(num_files_obj) ||
        !json_is_string(pipe_name_obj) || !json_is_integer(min_value_obj) ||
        !json_is_string(log_obj) || !json_is_string(n_gateway_obj) ||
        !json_is_string(i_gateway_obj) || !json_is_integer(ncores_obj) ||
        !json_is_integer(mul_factor_obj) || !json_is_string(cosmos_uri_obj) ||
        !json_is_string(cosmos_key_obj) ||
        !json_is_string(cosmos_db_name_obj) ||
        !json_is_string(cosmos_container_obj) ||
        !json_is_integer(max_value_obj)) {

        log_trace("validate_json_fields: Invalid config file format");
        return false;
    }

    return true;
}

static bool load_string_fields(config_t *config, json_t *root) {
    json_t *db_obj = json_object_get(root, "db");
    json_t *output_obj = json_object_get(root, "output");
    json_t *pipe_name_obj = json_object_get(root, "pipe_name");
    json_t *log_obj = json_object_get(root, "log");
    json_t *n_gateway_obj = json_object_get(root, "n_gateway");
    json_t *i_gateway_obj = json_object_get(root, "i_gateway");
    json_t *cosmos_uri_obj = json_object_get(root, "cosmos_uri");
    json_t *cosmos_key_obj = json_object_get(root, "cosmos_key");
    json_t *cosmos_db_name_obj = json_object_get(root, "cosmos_db_name");
    json_t *cosmos_container_obj = json_object_get(root, "cosmos_container");

    config->db = safe_string_duplicate(json_string_value(db_obj));
    config->output = safe_string_duplicate(json_string_value(output_obj));
    config->pipe_name = safe_string_duplicate(json_string_value(pipe_name_obj));
    config->log = safe_string_duplicate(json_string_value(log_obj));
    config->n_gateway = safe_string_duplicate(json_string_value(n_gateway_obj));
    config->i_gateway = safe_string_duplicate(json_string_value(i_gateway_obj));
    config->cosmos_uri =
        safe_string_duplicate(json_string_value(cosmos_uri_obj));
    config->cosmos_key =
        safe_string_duplicate(json_string_value(cosmos_key_obj));
    config->cosmos_db_name =
        safe_string_duplicate(json_string_value(cosmos_db_name_obj));
    config->cosmos_container =
        safe_string_duplicate(json_string_value(cosmos_container_obj));

    if (!config->db || !config->output || !config->pipe_name || !config->log ||
        !config->n_gateway || !config->i_gateway || !config->cosmos_uri ||
        !config->cosmos_key || !config->cosmos_db_name ||
        !config->cosmos_container) {

        log_trace(
            "load_string_fields: Failed to duplicate one or more string fields");
        return false;
    }

    return true;
}

static void load_integer_fields(config_t *config, json_t *root) {
    json_t *max_retries_obj = json_object_get(root, "max_retries");
    json_t *timeout_obj = json_object_get(root, "timeout");
    json_t *num_files_obj = json_object_get(root, "num_files");
    json_t *min_value_obj = json_object_get(root, "min_value");
    json_t *ncores_obj = json_object_get(root, "ncores");
    json_t *mul_factor_obj = json_object_get(root, "mul_factor");
    json_t *max_value_obj = json_object_get(root, "max_value");

    config->max_retries = json_integer_value(max_retries_obj);
    config->timeout = json_integer_value(timeout_obj);
    config->num_files = json_integer_value(num_files_obj);
    config->min_value = json_integer_value(min_value_obj);
    config->ncores = json_integer_value(ncores_obj);
    config->mul_factor = json_integer_value(mul_factor_obj);
    config->max_value = json_integer_value(max_value_obj);

    log_trace("load_integer_fields: max_retries=%d, timeout=%d, num_files=%d",
              config->max_retries, config->timeout, config->num_files);
    log_trace(
        "load_integer_fields: min_value=%d, max_value=%d, ncores=%d, mul_factor=%d",
        config->min_value, config->max_value, config->ncores,
        config->mul_factor);
}

static bool load_gateway_array(config_t *config, json_t *root) {
    json_t *gateways_array = json_object_get(root, "gateways");

    config->num_gateways = json_array_size(gateways_array);
    if (config->num_gateways <= 0) {
        log_trace("load_gateway_array: Empty gateways array");
        return false;
    }

    log_trace("load_gateway_array: Loading %d gateways", config->num_gateways);

    config->gateways = malloc(config->num_gateways * sizeof(char *));
    if (!config->gateways) {
        log_trace(
            "load_gateway_array: Failed to allocate memory for gateways array");
        return false;
    }

    for (size_t i = 0; i < config->num_gateways; i++) {
        json_t *gateway_obj = json_array_get(gateways_array, i);

        if (!json_is_string(gateway_obj)) {
            log_trace("load_gateway_array: Invalid gateway format at index %zu",
                      i);

            for (size_t j = 0; j < i; j++) {
                free(config->gateways[j]);
            }
            free(config->gateways);
            config->gateways = NULL;
            return false;
        }

        config->gateways[i] =
            safe_string_duplicate(json_string_value(gateway_obj));
        if (!config->gateways[i]) {
            log_trace(
                "load_gateway_array: Failed to duplicate gateway string at index %zu",
                i);

            for (size_t j = 0; j < i; j++) {
                free(config->gateways[j]);
            }
            free(config->gateways);
            config->gateways = NULL;
            return false;
        }

        log_trace("load_gateway_array: Gateway %zu: %s", i,
                  config->gateways[i]);
    }

    return true;
}

static void cleanup_config_on_error(config_t *config) {
    log_trace("cleanup_config_on_error: Cleaning up partially loaded config");

    free(config->db);
    free(config->output);
    free(config->log);
    free(config->pipe_name);
    free(config->n_gateway);
    free(config->i_gateway);
    free(config->cosmos_uri);
    free(config->cosmos_key);
    free(config->cosmos_db_name);
    free(config->cosmos_container);

    if (config->gateways) {
        for (int i = 0; i < config->num_gateways; i++) {
            free(config->gateways[i]);
        }
        free(config->gateways);
    }

    memset(config, 0, sizeof(config_t));
}

void config_read(const char *config_file, config_t *config) {
    if (!config_file || !config) {
        log_trace("config_read: Invalid parameters");
        exit(-1);
    }

    log_trace("config_read: Reading config file: %s", config_file);

    json_error_t error;
    json_t *root = json_load_file(config_file, 0, &error);

    if (!root) {
        log_trace("config_read: Failed to parse JSON config: %s", error.text);
        exit(-1);
    }

    if (!validate_json_fields(root)) {
        json_decref(root);
        exit(-1);
    }

    memset(config, 0, sizeof(config_t));

    if (!load_string_fields(config, root)) {
        cleanup_config_on_error(config);
        json_decref(root);
        exit(-1);
    }

    load_integer_fields(config, root);

    if (!load_gateway_array(config, root)) {
        cleanup_config_on_error(config);
        json_decref(root);
        exit(-1);
    }

    json_decref(root);

    log_trace("config_read: Configuration loaded successfully");
}