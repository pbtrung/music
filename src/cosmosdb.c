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
    if (copy)
        strcpy(copy, s);
    return copy;
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct mem *m = (struct mem *)userdata;
    char *p = realloc(m->data, m->len + total + 1);
    if (!p)
        return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = '\0';
    return total;
}

/* RFC1123 for header; we lowercase a copy for the string-to-sign */
static char *rfc1123_now(apr_pool_t *pool) {
    time_t now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);
    char *buf = apr_palloc(pool, 64);
    strftime(buf, 64, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
    return buf;
}

/* Build URL-encoded Authorization header value using OpenSSL HMAC-SHA256 */
static char *build_auth_token(CURL *curl, apr_pool_t *pool,
                              const config_t *config,
                              const char *verb,          /* e.g., "GET" */
                              const char *resource_type, /* e.g., "docs" */
                              const char *resource_link, /* exact-casing path */
                              const char *date_rfc1123)  /* header form */
{
    /* Lowercase components per spec (verb, resourceType, date) */
    char *verb_l = apr_pstrdup(pool, verb);
    char *rtype_l = apr_pstrdup(pool, resource_type);
    char *date_l = apr_pstrdup(pool, date_rfc1123);
    for (char *p = verb_l; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    for (char *p = rtype_l; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    for (char *p = date_l; *p; ++p)
        *p = (char)tolower((unsigned char)*p);

    /* String to sign: "{verb}\n{resourceType}\n{resourceLink}\n{date}\n\n" */
    char *to_sign = apr_psprintf(pool, "%s\n%s\n%s\n%s\n\n", verb_l, rtype_l,
                                 resource_link, date_l);

    /* Decode base64 key (MIME RFC2045) */
    int key_buf_len = apr_base64_decode_len(config->cosmos_key);
    unsigned char *key_raw = apr_palloc(pool, key_buf_len);
    int key_len = apr_base64_decode_binary(key_raw, config->cosmos_key);

    /* HMAC-SHA256 */
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), key_raw, key_len, (const unsigned char *)to_sign,
         strlen(to_sign), mac, &mac_len);

    /* Base64-encode signature */
    int b64_len = apr_base64_encode_len((int)mac_len);
    char *sig_b64 = apr_palloc(pool, b64_len);
    apr_base64_encode(sig_b64, (const char *)mac, (int)mac_len);

    /* Entire token "type=master&ver=1.0&sig=..." must be URL-encoded */
    char *token_plain =
        apr_psprintf(pool, "type=master&ver=1.0&sig=%s", sig_b64);
    char *token_enc =
        curl_easy_escape(curl, token_plain, 0); /* alloc by libcurl */

    /* Copy into APR pool memory and free curl allocation */
    char *auth = apr_pstrdup(pool, token_enc);
    curl_free(token_enc);
    return auth;
}

json_t *cosmosdb_get_item(apr_pool_t *pool, const config_t *config) {
    log_trace("cosmosdb_get_item: start");
    int attempt = 0;

    while (attempt < config->max_retries) {
        attempt++;
        log_trace("cosmosdb_get_item: start loop (attempt %d)", attempt);

        // Generate random track_id
        int *random_index = util_random_ints(1, 1, config->max_value);
        char *track_id = apr_psprintf(pool, "%d", random_index[0]);
        free(random_index);
        log_trace("cosmosdb_get_item: track_id %s", track_id);

        char *resource_link = apr_psprintf(pool, "dbs/%s/colls/%s/docs/%s",
                                           config->cosmos_db_name,
                                           config->cosmos_container, track_id);

        CURL *curl = curl_easy_init();
        if (!curl) {
            fprintf(stderr, "Failed to init curl\n");
            return NULL;
        }

        const char *xms_version = "2018-12-31";
        char *xms_date = rfc1123_now(pool);

        char *auth = build_auth_token(curl, pool, config, "GET", "docs",
                                      resource_link, xms_date);

        char *url =
            apr_psprintf(pool, "%s/%s", config->cosmos_uri, resource_link);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(
            headers, apr_psprintf(pool, "x-ms-date: %s", xms_date));
        headers = curl_slist_append(
            headers, apr_psprintf(pool, "x-ms-version: %s", xms_version));
        headers = curl_slist_append(
            headers, apr_psprintf(pool, "authorization: %s", auth));
        headers = curl_slist_append(
            headers,
            apr_psprintf(pool, "x-ms-documentdb-partitionkey: [\"%s\"]",
                         track_id));

        struct mem resp = {.data = NULL, .len = 0};
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            fprintf(stderr, "curl error: %s\n", curl_easy_strerror(rc));
            free(resp.data);
            return NULL;
        }

        json_error_t jerr;
        json_t *root = json_loads(resp.data ? resp.data : "", 0, &jerr);
        free(resp.data);

        if (!root) {
            fprintf(stderr, "JSON parse error: %s (line %d)\n", jerr.text,
                    jerr.line);
            return NULL;
        }

        // Check if it's a "NotFound" error
        json_t *code = json_object_get(root, "code");
        if (json_is_string(code) &&
            strcmp(json_string_value(code), "NotFound") == 0) {
            json_decref(root);
            // clang-format off
            log_trace("cosmosdb_get_item: try again with a new random track_id (got %s)", track_id);
            // clang-format on
            continue;
        }

        return root;
    }

    fprintf(stderr, "cosmosdb_get_item: Max retries reached\n");
    return NULL;
}

void cosmosdb_file_info_init(file_info_t *info, json_t *doc, config_t *config) {
    if (!info || !doc || !json_is_object(doc))
        return;

    // track_name
    json_t *track_name = json_object_get(doc, "track_name");
    if (json_is_string(track_name)) {
        info->track_name = strdup_safe(json_string_value(track_name));
    }

    // album.path
    json_t *album = json_object_get(doc, "album");
    if (json_is_object(album)) {
        json_t *path = json_object_get(album, "path");
        if (json_is_string(path)) {
            info->album_path = strdup_safe(json_string_value(path));
        }
    }

    info->extension = util_get_extension(info->track_name);
    info->filename = util_get_filename_with_extension(info->track_name);
    info->config = config;
    info->file_download_status = DOWNLOAD_PENDING;

    // track_id
    json_t *track_id = json_object_get(doc, "track_id");
    if (json_is_integer(track_id)) {
        info->track_id = (int)json_integer_value(track_id);
    }

    // cids array
    json_t *cids = json_object_get(doc, "cids");
    if (json_is_array(cids)) {
        info->num_cids = json_array_size(cids);
        info->cids = calloc(info->num_cids, sizeof(char *));
        info->cid_download_status =
            calloc(info->num_cids, sizeof(enum download_status));
        for (int i = 0; i < info->num_cids; i++) {
            json_t *cid_item = json_array_get(cids, i);
            if (json_is_string(cid_item)) {
                info->cids[i] = strdup_safe(json_string_value(cid_item));
            }
            info->cid_download_status[i] = DOWNLOAD_PENDING;
        }
    }
}
