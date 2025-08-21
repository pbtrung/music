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

#include "const.h"
#include "cosmosdb.h"
#include "log.h"
#include "utils.h"

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} http_response_t;

typedef struct {
    const config_t *config;
    apr_pool_t *pool;
    CURL *curl;
    char *auth_token;
    char *date_header;
} cosmos_request_ctx_t;

static void http_response_init(http_response_t *resp) {
    if (!resp)
        return;

    resp->data = NULL;
    resp->len = 0;
    resp->capacity = 0;
}

static void http_response_cleanup(http_response_t *resp) {
    if (!resp)
        return;

    free(resp->data);
    resp->data = NULL;
    resp->len = 0;
    resp->capacity = 0;
}

static size_t http_write_callback(void *ptr, size_t size, size_t nmemb,
                                  void *userdata) {
    size_t total_size = size * nmemb;
    http_response_t *resp = (http_response_t *)userdata;

    if (!resp || !ptr)
        return 0;

    size_t required_capacity = resp->len + total_size + 1;
    if (required_capacity > resp->capacity) {
        size_t new_capacity = required_capacity * 2;
        char *new_data = realloc(resp->data, new_capacity);

        if (!new_data) {
            log_trace(
                "http_write_callback: Failed to allocate memory for HTTP response");
            return 0;
        }

        resp->data = new_data;
        resp->capacity = new_capacity;
    }

    memcpy(resp->data + resp->len, ptr, total_size);
    resp->len += total_size;
    resp->data[resp->len] = '\0';

    return total_size;
}

static char *create_rfc1123_timestamp(apr_pool_t *pool) {
    if (!pool)
        return NULL;

    time_t now = time(NULL);
    struct tm gmt;

    if (!gmtime_r(&now, &gmt)) {
        log_trace("create_rfc1123_timestamp: Failed to convert time to GMT");
        return NULL;
    }

    char *buffer = apr_palloc(pool, RFC1123_BUFFER_SIZE);
    if (!buffer)
        return NULL;

    size_t result = strftime(buffer, RFC1123_BUFFER_SIZE,
                             "%a, %d %b %Y %H:%M:%S GMT", &gmt);

    return (result > 0) ? buffer : NULL;
}

static unsigned char *
decode_base64_key(apr_pool_t *pool, const char *encoded_key, int *decoded_len) {
    if (!pool || !encoded_key || !decoded_len) {
        log_trace(
            "decode_base64_key: Invalid parameters for base64 key decoding");
        return NULL;
    }

    int buffer_len = apr_base64_decode_len(encoded_key);
    if (buffer_len <= 0) {
        log_trace("decode_base64_key: Invalid base64 encoded key length");
        return NULL;
    }

    unsigned char *decoded = apr_palloc(pool, buffer_len);
    if (!decoded) {
        log_trace(
            "decode_base64_key: Failed to allocate memory for decoded key");
        return NULL;
    }

    *decoded_len = apr_base64_decode_binary(decoded, encoded_key);
    if (*decoded_len <= 0) {
        log_trace("decode_base64_key: Failed to decode base64 key");
        return NULL;
    }

    return decoded;
}

static char *create_hmac_signature(apr_pool_t *pool, const unsigned char *key,
                                   int key_len, const char *message) {
    if (!pool || !key || !message || key_len <= 0) {
        log_trace(
            "create_hmac_signature: Invalid parameters for HMAC signature creation");
        return NULL;
    }

    unsigned char mac[MAX_MAC_SIZE];
    unsigned int mac_len = 0;

    if (!HMAC(EVP_sha256(), key, key_len, (const unsigned char *)message,
              strlen(message), mac, &mac_len)) {
        log_trace("create_hmac_signature: HMAC computation failed");
        return NULL;
    }

    int encoded_len = apr_base64_encode_len(mac_len);
    char *encoded = apr_palloc(pool, encoded_len);
    if (!encoded) {
        log_trace(
            "create_hmac_signature: Failed to allocate memory for encoded signature");
        return NULL;
    }

    apr_base64_encode(encoded, (const char *)mac, (int)mac_len);
    return encoded;
}

static char *build_signature_payload(apr_pool_t *pool, const char *http_verb,
                                     const char *resource_type,
                                     const char *resource_link,
                                     const char *timestamp) {
    if (!pool || !http_verb || !resource_type || !resource_link || !timestamp) {
        log_trace(
            "build_signature_payload: Invalid parameters for signature payload");
        return NULL;
    }

    char *verb_lower = apr_pstrdup(pool, http_verb);
    char *type_lower = apr_pstrdup(pool, resource_type);
    char *date_lower = apr_pstrdup(pool, timestamp);

    if (!verb_lower || !type_lower || !date_lower) {
        log_trace(
            "build_signature_payload: Failed to duplicate strings for signature payload");
        return NULL;
    }

    util_to_lower(verb_lower);
    util_to_lower(type_lower);
    util_to_lower(date_lower);

    return apr_psprintf(pool, "%s\n%s\n%s\n%s\n\n", verb_lower, type_lower,
                        resource_link, date_lower);
}

static char *create_auth_token(cosmos_request_ctx_t *ctx, const char *http_verb,
                               const char *resource_type,
                               const char *resource_link) {
    if (!ctx || !ctx->pool || !ctx->config || !http_verb || !resource_type ||
        !resource_link) {
        log_trace("create_auth_token: Invalid context for auth token creation");
        return NULL;
    }

    log_trace(
        "create_auth_token: Creating auth token: verb=%s, type=%s, link=%s",
        http_verb, resource_type, resource_link);

    char *payload = build_signature_payload(ctx->pool, http_verb, resource_type,
                                            resource_link, ctx->date_header);
    if (!payload) {
        log_trace("create_auth_token: Failed to build signature payload");
        return NULL;
    }

    log_trace("create_auth_token: Signature payload: '%s'", payload);

    int key_len;
    unsigned char *decoded_key =
        decode_base64_key(ctx->pool, ctx->config->cosmos_key, &key_len);
    if (!decoded_key) {
        log_trace("create_auth_token: Failed to decode CosmosDB key");
        return NULL;
    }

    char *signature =
        create_hmac_signature(ctx->pool, decoded_key, key_len, payload);
    if (!signature) {
        log_trace("create_auth_token: Failed to create HMAC signature");
        return NULL;
    }

    char *token_plain =
        apr_psprintf(ctx->pool, "type=master&ver=1.0&sig=%s", signature);
    if (!token_plain) {
        log_trace("create_auth_token: Failed to create plain auth token");
        return NULL;
    }

    char *token_encoded = curl_easy_escape(ctx->curl, token_plain, 0);
    if (!token_encoded) {
        log_trace("create_auth_token: Failed to URL encode auth token");
        return NULL;
    }

    char *result = apr_pstrdup(ctx->pool, token_encoded);
    curl_free(token_encoded);

    log_trace("create_auth_token: Auth token created successfully");
    return result;
}

static struct curl_slist *build_http_headers(cosmos_request_ctx_t *ctx,
                                             const char *partition_key) {
    if (!ctx || !ctx->pool || !partition_key) {
        log_trace("build_http_headers: Invalid parameters for HTTP headers");
        return NULL;
    }

    struct curl_slist *headers = NULL;

    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(
        headers, apr_psprintf(ctx->pool, "x-ms-date: %s", ctx->date_header));
    headers =
        curl_slist_append(headers, apr_psprintf(ctx->pool, "x-ms-version: %s",
                                                COSMOS_API_VERSION));
    headers = curl_slist_append(
        headers, apr_psprintf(ctx->pool, "authorization: %s", ctx->auth_token));
    headers = curl_slist_append(
        headers,
        apr_psprintf(ctx->pool, "x-ms-documentdb-partitionkey: [\"%s\"]",
                     partition_key));

    return headers;
}

static struct curl_slist *setup_curl_get_request(cosmos_request_ctx_t *ctx, const char *url,
                                   const char *partition_key,
                                   http_response_t *response) {
    if (!ctx || !ctx->curl || !url || !partition_key || !response) {
        log_trace(
            "setup_curl_get_request: Invalid parameters for CURL GET request setup");
        return false;
    }

    log_trace("setup_curl_get_request: Setting up CURL request: url=%s", url);

    struct curl_slist *headers = build_http_headers(ctx, partition_key);
    if (!headers) {
        log_trace("setup_curl_get_request: Failed to build HTTP headers");
        return false;
    }

    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(ctx->curl, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(ctx->curl, CURLOPT_FOLLOWLOCATION, 1L);

    return headers;
}

static json_t *fetch_cosmos_item(cosmos_request_ctx_t *ctx,
                                 const char *track_id) {
    if (!ctx || !ctx->pool || !ctx->config || !track_id) {
        log_trace(
            "fetch_cosmos_item: Invalid parameters for CosmosDB item fetch");
        return NULL;
    }

    log_trace("fetch_cosmos_item: Fetching CosmosDB item: track_id=%s",
              track_id);

    char *resource_link = apr_psprintf(ctx->pool, "dbs/%s/colls/%s/docs/%s",
                                       ctx->config->cosmos_db_name,
                                       ctx->config->cosmos_container, track_id);
    if (!resource_link) {
        log_trace("fetch_cosmos_item: Failed to build resource link");
        return NULL;
    }

    ctx->auth_token = create_auth_token(ctx, "GET", "docs", resource_link);
    if (!ctx->auth_token) {
        log_trace("fetch_cosmos_item: Failed to create auth token");
        return NULL;
    }

    char *url = apr_psprintf(ctx->pool, "%s/%s", ctx->config->cosmos_uri,
                             resource_link);
    if (!url) {
        log_trace("fetch_cosmos_item: Failed to build request URL");
        return NULL;
    }

    http_response_t response;
    http_response_init(&response);

    struct curl_slist *headers = setup_curl_get_request(ctx, url, track_id, &response);
    if (!headers) {
        log_trace("fetch_cosmos_item: Failed to setup CURL request");
        http_response_cleanup(&response);
        return NULL;
    }

    CURLcode curl_result = curl_easy_perform(ctx->curl);
    curl_slist_free_all(headers);
    long http_status = 0;
    curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &http_status);

    log_trace(
        "fetch_cosmos_item: HTTP request completed: status=%ld, curl_result=%s",
        http_status, curl_easy_strerror(curl_result));

    if (curl_result != CURLE_OK) {
        log_trace("fetch_cosmos_item: CURL request failed: %s",
                  curl_easy_strerror(curl_result));
        http_response_cleanup(&response);
        return NULL;
    }

    if (http_status != HTTP_STATUS_OK) {
        log_trace("fetch_cosmos_item: HTTP request returned status %ld",
                  http_status);
        http_response_cleanup(&response);
        return NULL;
    }

    if (!response.data || response.len == 0) {
        log_trace("fetch_cosmos_item: Empty response from CosmosDB");
        http_response_cleanup(&response);
        return NULL;
    }

    json_error_t json_error;
    json_t *document = json_loads(response.data, 0, &json_error);
    http_response_cleanup(&response);

    if (!document) {
        log_trace("fetch_cosmos_item: JSON parse error: %s (line %d)",
                  json_error.text, json_error.line);
        return NULL;
    }

    return document;
}

static bool is_not_found_response(json_t *document) {
    if (!document)
        return false;

    json_t *code_field = json_object_get(document, "code");
    if (!json_is_string(code_field))
        return false;

    return (strcmp(json_string_value(code_field), "NotFound") == 0);
}

static char *generate_random_track_id(apr_pool_t *pool,
                                      const config_t *config) {
    if (!pool || !config) {
        log_trace(
            "generate_random_track_id: Invalid parameters for random track ID generation");
        return NULL;
    }

    int *random_values = util_rand_ints(1, 1, config->max_value);
    if (!random_values) {
        log_trace("generate_random_track_id: Failed to generate random values");
        return NULL;
    }

    char *track_id = apr_psprintf(pool, "%d", random_values[0]);
    free(random_values);

    return track_id;
}

json_t *cosmosdb_get_item(apr_pool_t *pool, const config_t *config) {
    if (!pool || !config) {
        log_trace(
            "cosmosdb_get_item: Invalid parameters for CosmosDB get item");
        return NULL;
    }

    log_trace("cosmosdb_get_item: Starting CosmosDB item retrieval");

    cosmos_request_ctx_t ctx = {0};
    ctx.config = config;
    ctx.pool = pool;
    ctx.curl = curl_easy_init();

    if (!ctx.curl) {
        log_trace("cosmosdb_get_item: Failed to initialize CURL");
        return NULL;
    }

    ctx.date_header = create_rfc1123_timestamp(pool);
    if (!ctx.date_header) {
        log_trace("cosmosdb_get_item: Failed to create timestamp");
        curl_easy_cleanup(ctx.curl);
        return NULL;
    }

    json_t *result = NULL;

    for (int attempt = 1; attempt <= config->max_retries; attempt++) {
        log_trace("cosmosdb_get_item: CosmosDB retrieval attempt %d of %d",
                  attempt, config->max_retries);

        char *track_id = generate_random_track_id(pool, config);
        if (!track_id) {
            log_trace("cosmosdb_get_item: Failed to generate random track ID");
            continue;
        }

        log_trace("cosmosdb_get_item: Trying track_id=%s", track_id);

        json_t *document = fetch_cosmos_item(&ctx, track_id);
        if (!document) {
            log_trace(
                "cosmosdb_get_item: Failed to fetch document for track_id=%s",
                track_id);
            continue;
        }

        if (is_not_found_response(document)) {
            log_trace(
                "cosmosdb_get_item: Document not found for track_id=%s, retrying",
                track_id);
            json_decref(document);
            continue;
        }

        result = document;
        break;
    }

    curl_easy_cleanup(ctx.curl);

    if (!result) {
        log_trace(
            "cosmosdb_get_item: Failed to retrieve CosmosDB item after %d attempts",
            config->max_retries);
    } else {
        log_trace("cosmosdb_get_item: Successfully retrieved CosmosDB item");
    }

    return result;
}
