#include <jansson.h>

#include "cosmosdb.h"
#include "file_info.h"
#include "log.h"
#include "utils.h"

static void extract_string_field(char **destination, json_t *json_object,
                                 const char *field_name) {
    if (!destination || !json_object || !field_name)
        return;

    if (json_is_string(json_object)) {
        *destination =
            util_safe_strdup(json_string_value(json_object), "json_object");
        log_trace("extract_string_field: Extracted field %s: %s", field_name,
                  *destination ? *destination : "(null)");
    }
}

static void initialize_cid_array(file_info_t *info, json_t *cids_array) {
    if (!info || !json_is_array(cids_array)) {
        log_trace("initialize_cid_array: Invalid CIDs array or info structure");
        return;
    }

    info->num_cids = (int)json_array_size(cids_array);
    if (info->num_cids <= 0) {
        log_trace("initialize_cid_array: Empty CIDs array");
        return;
    }

    log_trace("initialize_cid_array: Initializing %d CIDs", info->num_cids);

    info->cids = calloc(info->num_cids, sizeof(char *));
    info->cid_download_status =
        calloc(info->num_cids, sizeof(enum download_status));

    if (!info->cids || !info->cid_download_status) {
        log_trace("initialize_cid_array: Failed to allocate memory for CIDs");
        free(info->cids);
        free(info->cid_download_status);
        info->cids = NULL;
        info->cid_download_status = NULL;
        info->num_cids = 0;
        return;
    }

    for (int i = 0; i < info->num_cids; i++) {
        json_t *cid_element = json_array_get(cids_array, i);
        if (cid_element)
            extract_string_field(&info->cids[i], cid_element, "cid");
        info->cid_download_status[i] = DOWNLOAD_PENDING;
    }
}

static void file_info_from_json(file_info_t *info, json_t *document,
                                config_t *config) {
    if (!info || !document || !config) {
        log_trace(
            "file_info_from_json: Invalid parameters for file info initialization");
        return;
    }

    log_trace(
        "file_info_from_json: Initializing file info from CosmosDB document");

    memset(info, 0, sizeof(file_info_t));
    info->config = config;
    info->file_download_status = DOWNLOAD_PENDING;

    json_t *track_name_field = json_object_get(document, "track_name");
    extract_string_field(&info->track_name, track_name_field, "track_name");

    json_t *album_object = json_object_get(document, "album");
    if (json_is_object(album_object)) {
        json_t *album_path_field = json_object_get(album_object, "path");
        extract_string_field(&info->album_path, album_path_field, "album_path");
    }

    json_t *track_id_field = json_object_get(document, "track_id");
    if (json_is_integer(track_id_field)) {
        info->track_id = (int)json_integer_value(track_id_field);
        log_trace("file_info_from_json: Track ID: %d", info->track_id);
    }

    if (info->track_name) {
        info->extension = util_get_ext(info->track_name);
        info->filename = util_gen_filename(info->track_name);
    }

    json_t *cids_field = json_object_get(document, "cids");
    initialize_cid_array(info, cids_field);

    log_trace("file_info_from_json: File info initialization completed");
}

file_info_t *file_info_create(apr_pool_t *subpool, config_t *cfg, json_t *doc) {
    if (!subpool || !cfg || !doc) {
        log_trace("file_info_create: Invalid parameters");
        return NULL;
    }

    file_info_t *info = apr_palloc(subpool, sizeof(file_info_t));
    if (!info) {
        log_trace("file_info_create: Memory allocation failed");
        return NULL;
    }

    // Initialize the structure
    memset(info, 0, sizeof(file_info_t));

    file_info_from_json(info, doc, cfg);

    apr_pool_cleanup_register(subpool, info, file_info_free,
                              apr_pool_cleanup_null);

    return info;
}

apr_status_t file_info_free(void *data) {
    log_trace("file_info_free: start");
    if (!data) {
        log_trace("file_info_free: NULL data pointer");
        return APR_SUCCESS;
    }

    file_info_t *info = (file_info_t *)data;
    free(info->track_name);
    free(info->album_path);
    free(info->filename);
    free(info->extension);

    if (info->cids) {
        for (int j = 0; j < info->num_cids; ++j)
            free(info->cids[j]);
        free(info->cids);
    }
    free(info->cid_download_status);
    log_trace("file_info_free: finish");
    return APR_SUCCESS;
}
