#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <apr_base64.h>
#include <apr_strings.h>
#include <curl/curl.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "cosmosdb.h"
#include "log.h"
#include "utils.h"

struct mem {
    char *data;
    size_t len;
};

static char *strdup_safe(const char *s) {
    if (!s)
        return NULL;
    char *copy = malloc(strlen(s) + 1);
    return copy ? strcpy(copy, s) : NULL;
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct mem *m = userdata;
    char *p = realloc(m->data, m->len + total + 1);
    if (!p)
        return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = '\0';
    return total;
}

static char *rfc1123_now(apr_pool_t *pool) {
    time_t now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);
    char *buf = apr_palloc(pool, 64);
    strftime(buf, 64, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return buf;
}

static void lowercase(char *s) {
    for (; *s; ++s)
        *s = (char)tolower((unsigned char)*s);
}

static unsigned char *decode_key(apr_pool_t *pool, const char *key,
                                 int *out_len) {
    int buf_len = apr_base64_decode_len(key);
    unsigned char *raw = apr_palloc(pool, buf_len);
    *out_len = apr_base64_decode_binary(raw, key);
    return raw;
}

static char *hmac_sha256_b64(apr_pool_t *pool, const unsigned char *key,
                             int key_len, const char *msg) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), key, key_len, (const unsigned char *)msg, strlen(msg),
         mac, &mac_len);
    char *out = apr_palloc(pool, apr_base64_encode_len(mac_len));
    apr_base64_encode(out, (const char *)mac, (int)mac_len);
    return out;
}

static char *build_auth_token(CURL *curl, apr_pool_t *pool,
                              const config_t *config, const char *verb,
                              const char *resource_type,
                              const char *resource_link,
                              const char *date_rfc1123) {
    log_trace("build_auth_token: verb=%s, resource_type=%s, link=%s", verb,
              resource_type, resource_link);

    char *verb_l = apr_pstrdup(pool, verb);
    lowercase(verb_l);
    char *rtype_l = apr_pstrdup(pool, resource_type);
    lowercase(rtype_l);
    char *date_l = apr_pstrdup(pool, date_rfc1123);
    lowercase(date_l);

    char *to_sign = apr_psprintf(pool, "%s\n%s\n%s\n%s\n\n", verb_l, rtype_l,
                                 resource_link, date_l);
    log_trace("build_auth_token: to_sign='%s'", to_sign);

    int key_len;
    unsigned char *key_raw = decode_key(pool, config->cosmos_key, &key_len);
    char *sig_b64 = hmac_sha256_b64(pool, key_raw, key_len, to_sign);

    char *token_plain =
        apr_psprintf(pool, "type=master&ver=1.0&sig=%s", sig_b64);
    char *token_enc = curl_easy_escape(curl, token_plain, 0);
    char *auth = apr_pstrdup(pool, token_enc);
    curl_free(token_enc);

    log_trace("build_auth_token: done");
    return auth;
}

static CURL *setup_curl_request(apr_pool_t *pool, const char *url,
                                const char *auth, const char *date,
                                const char *version, const char *partition_key,
                                struct mem *resp) {
    log_trace("setup_curl_request: url=%s", url);

    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers =
        curl_slist_append(headers, apr_psprintf(pool, "x-ms-date: %s", date));
    headers = curl_slist_append(
        headers, apr_psprintf(pool, "x-ms-version: %s", version));
    headers = curl_slist_append(headers,
                                apr_psprintf(pool, "authorization: %s", auth));
    headers = curl_slist_append(
        headers, apr_psprintf(pool, "x-ms-documentdb-partitionkey: [\"%s\"]",
                              partition_key));

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    return curl;
}

static json_t *try_get_item(apr_pool_t *pool, const config_t *config,
                            const char *track_id) {
    log_trace("try_get_item: track_id=%s", track_id);

    char *resource_link =
        apr_psprintf(pool, "dbs/%s/colls/%s/docs/%s", config->cosmos_db_name,
                     config->cosmos_container, track_id);
    const char *xms_version = "2018-12-31";
    char *xms_date = rfc1123_now(pool);

    CURL *curl = curl_easy_init();
    char *auth = build_auth_token(curl, pool, config, "GET", "docs",
                                  resource_link, xms_date);
    char *url = apr_psprintf(pool, "%s/%s", config->cosmos_uri, resource_link);

    struct mem resp = {0};
    CURL *req = setup_curl_request(pool, url, auth, xms_date, xms_version,
                                   track_id, &resp);
    if (!req)
        return NULL;

    CURLcode rc = curl_easy_perform(req);
    long status = 0;
    curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(req);

    log_trace("try_get_item: HTTP status=%ld", status);

    if (rc != CURLE_OK) {
        log_trace("try_get_item: curl error=%s", curl_easy_strerror(rc));
        free(resp.data);
        return NULL;
    }

    json_error_t jerr;
    json_t *root = json_loads(resp.data ? resp.data : "", 0, &jerr);
    free(resp.data);

    if (!root) {
        log_trace("try_get_item: JSON parse error=%s (line %d)", jerr.text,
                  jerr.line);
        return NULL;
    }
    return root;
}

json_t *cosmosdb_get_item(apr_pool_t *pool, const config_t *config) {
    log_trace("cosmosdb_get_item: start");

    for (int attempt = 1; attempt <= config->max_retries; attempt++) {
        log_trace("cosmosdb_get_item: attempt %d", attempt);

        int *random_index = util_random_ints(1, 1, config->max_value);
        char *track_id = apr_psprintf(pool, "%d", random_index[0]);
        free(random_index);
        log_trace("cosmosdb_get_item: track_id=%s", track_id);

        json_t *root = try_get_item(pool, config, track_id);
        if (!root)
            continue;

        json_t *code = json_object_get(root, "code");
        if (json_is_string(code) &&
            strcmp(json_string_value(code), "NotFound") == 0) {
            log_trace("cosmosdb_get_item: NotFound for track_id=%s, retrying",
                      track_id);
            json_decref(root);
            continue;
        }
        return root;
    }

    log_trace("cosmosdb_get_item: Max retries reached");
    return NULL;
}

static void set_string_field(char **dst, json_t *obj, const char *field_name) {
    if (json_is_string(obj)) {
        *dst = strdup_safe(json_string_value(obj));
        log_trace("set_string_field: %s=%s", field_name, *dst);
    }
}

static void set_cids(file_info_t *info, json_t *cids) {
    if (!json_is_array(cids))
        return;
    info->num_cids = json_array_size(cids);
    info->cids = calloc(info->num_cids, sizeof(char *));
    info->cid_download_status =
        calloc(info->num_cids, sizeof(enum download_status));

    log_trace("set_cids: num_cids=%d", info->num_cids);

    for (int i = 0; i < info->num_cids; i++) {
        set_string_field(&info->cids[i], json_array_get(cids, i), "cid");
        info->cid_download_status[i] = DOWNLOAD_PENDING;
    }
}

void cosmosdb_file_info_init(file_info_t *info, json_t *doc, config_t *config) {
    if (!info || !doc)
        return;

    set_string_field(&info->track_name, json_object_get(doc, "track_name"),
                     "track_name");

    json_t *album = json_object_get(doc, "album");
    if (json_is_object(album))
        set_string_field(&info->album_path, json_object_get(album, "path"),
                         "album_path");

    info->extension = util_get_extension(info->track_name);
    info->filename = util_get_filename_with_extension(info->track_name);
    info->config = config;
    info->file_download_status = DOWNLOAD_PENDING;

    json_t *track_id = json_object_get(doc, "track_id");
    if (json_is_integer(track_id)) {
        info->track_id = (int)json_integer_value(track_id);
        log_trace("cosmosdb_file_info_init: track_id=%d", info->track_id);
    }

    set_cids(info, json_object_get(doc, "cids"));
}
