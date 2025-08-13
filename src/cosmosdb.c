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

struct mem {
    char *data;
    size_t len;
};

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
                              const cosmosdb_credentials_t *creds,
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
    int key_buf_len = apr_base64_decode_len(creds->master_key);
    unsigned char *key_raw = apr_palloc(pool, key_buf_len);
    int key_len = apr_base64_decode_binary(key_raw, creds->master_key);

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

int cosmosdb_init(apr_pool_t **pool) {
    if (apr_initialize() != APR_SUCCESS)
        return -1;
    if (apr_pool_create(pool, NULL) != APR_SUCCESS)
        return -1;
    if (curl_global_init(CURL_GLOBAL_ALL) != 0)
        return -1;
    return 0;
}

void cosmosdb_cleanup(apr_pool_t *pool) {
    curl_global_cleanup();
    apr_pool_destroy(pool);
    apr_terminate();
}

json_t *cosmosdb_get_item(apr_pool_t *pool, const cosmosdb_credentials_t *creds,
                          const char *resource_link, const char *resource_type,
                          const char *partition_key) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to init curl\n");
        return NULL;
    }

    const char *xms_version = "2018-12-31"; /* REST API version */
    char *xms_date = rfc1123_now(pool);

    /* Build auth AFTER we have curl, so we can use its escaper */
    char *auth = build_auth_token(curl, pool, creds, "GET", resource_type,
                                  resource_link, xms_date);

    /* URL: https://{account}.documents.azure.com/{resource_link} */
    char *url = apr_psprintf(pool, "https://%s.documents.azure.com/%s",
                             creds->account, resource_link);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers,
                                apr_psprintf(pool, "x-ms-date: %s", xms_date));
    headers = curl_slist_append(
        headers, apr_psprintf(pool, "x-ms-version: %s", xms_version));
    headers = curl_slist_append(headers,
                                apr_psprintf(pool, "authorization: %s", auth));

    if (partition_key) {
        /* PK must be a JSON array string: ["value"] (or multiple components if
         * hierarchical) */
        headers = curl_slist_append(
            headers,
            apr_psprintf(pool, "x-ms-documentdb-partitionkey: [\"%s\"]",
                         partition_key));
    }

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

    /* Parse JSON (even on errors, service returns JSON with "code"/"message")
     */
    json_error_t jerr;
    json_t *root = json_loads(resp.data ? resp.data : "", 0, &jerr);
    free(resp.data);

    if (!root) {
        fprintf(stderr, "JSON parse error: %s (line %d)\n", jerr.text,
                jerr.line);
        return NULL;
    }

    /* Optional: you can inspect status here if you want to gate success */
    (void)status;
    return root;
}
