#include <stdio.h>
#include <string.h>
#include <time.h>

#include <apr_strings.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "aws_dynamodb.h"

#define AWS_SERVICE "dynamodb"
#define ENDPOINT_FMT "https://dynamodb.%s.amazonaws.com"
#define HOST_FMT "dynamodb.%s.amazonaws.com"

static void to_hex(const unsigned char *hash, size_t len, char *out) {
    for (size_t i = 0; i < len; i++)
        sprintf(out + (i * 2), "%02x", hash[i]);
}

static void hmac_sha256(const void *key, int keylen, const unsigned char *data,
                        size_t datalen, unsigned char *out) {
    unsigned int len = SHA256_DIGEST_LENGTH;
    HMAC(EVP_sha256(), key, keylen, data, datalen, out, &len);
}

static void get_signature_key(unsigned char *out, const char *key,
                              const char *dateStamp, const char *regionName,
                              const char *serviceName) {
    unsigned char kDate[SHA256_DIGEST_LENGTH];
    unsigned char kRegion[SHA256_DIGEST_LENGTH];
    unsigned char kService[SHA256_DIGEST_LENGTH];
    unsigned char kSigning[SHA256_DIGEST_LENGTH];

    char kSecret[128];
    sprintf(kSecret, "AWS4%s", key);

    hmac_sha256(kSecret, strlen(kSecret), (unsigned char *)dateStamp,
                strlen(dateStamp), kDate);
    hmac_sha256(kDate, SHA256_DIGEST_LENGTH, (unsigned char *)regionName,
                strlen(regionName), kRegion);
    hmac_sha256(kRegion, SHA256_DIGEST_LENGTH, (unsigned char *)serviceName,
                strlen(serviceName), kService);
    hmac_sha256(kService, SHA256_DIGEST_LENGTH, (unsigned char *)"aws4_request",
                12, kSigning);

    memcpy(out, kSigning, SHA256_DIGEST_LENGTH);
}

int aws_dynamodb_init(apr_pool_t **pool) {
    if (apr_initialize() != APR_SUCCESS) {
        fprintf(stderr, "Failed to initialize APR\n");
        return -1;
    }
    if (apr_pool_create(pool, NULL) != APR_SUCCESS) {
        fprintf(stderr, "Failed to create APR pool\n");
        return -1;
    }
    curl_global_init(CURL_GLOBAL_ALL);
    return 0;
}

void aws_dynamodb_cleanup(apr_pool_t *pool) {
    apr_pool_destroy(pool);
    curl_global_cleanup();
    apr_terminate();
}

int aws_dynamodb_request(apr_pool_t *pool, const aws_credentials_t *creds,
                         const char *operation, const char *payload) {
    /* Step 1: Dates */
    time_t t = time(NULL);
    struct tm gm;
    gmtime_r(&t, &gm);
    char amz_date[17], date_stamp[9];
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &gm);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", &gm);

    /* Step 2: Payload hash */
    char payload_hash_hex[65];
    {
        unsigned char payload_hash[SHA256_DIGEST_LENGTH];
        SHA256((unsigned char *)payload, strlen(payload), payload_hash);
        to_hex(payload_hash, SHA256_DIGEST_LENGTH, payload_hash_hex);
        payload_hash_hex[64] = '\0';
    }

    /* Step 3: Canonical request */
    char *host = apr_psprintf(pool, HOST_FMT, creds->region);
    char *canonical_headers =
        apr_psprintf(pool,
                     "content-type:application/x-amz-json-1.0\n"
                     "host:%s\n"
                     "x-amz-date:%s\n"
                     "x-amz-target:DynamoDB_20120810.%s\n",
                     host, amz_date, operation);

    const char *signed_headers = "content-type;host;x-amz-date;x-amz-target";
    char *canonical_request =
        apr_psprintf(pool, "POST\n/\n\n%s\n%s\n%s", canonical_headers,
                     signed_headers, payload_hash_hex);

    unsigned char cr_hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)canonical_request, strlen(canonical_request),
           cr_hash);
    char cr_hash_hex[65];
    to_hex(cr_hash, SHA256_DIGEST_LENGTH, cr_hash_hex);

    /* Step 4: String to sign */
    char *credential_scope = apr_psprintf(
        pool, "%s/%s/%s/aws4_request", date_stamp, creds->region, AWS_SERVICE);
    char *string_to_sign =
        apr_psprintf(pool, "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date,
                     credential_scope, cr_hash_hex);

    /* Step 5: Signing */
    unsigned char signing_key[SHA256_DIGEST_LENGTH];
    get_signature_key(signing_key, creds->secret_key, date_stamp, creds->region,
                      AWS_SERVICE);

    unsigned char signature_bin[SHA256_DIGEST_LENGTH];
    hmac_sha256(signing_key, SHA256_DIGEST_LENGTH,
                (unsigned char *)string_to_sign, strlen(string_to_sign),
                signature_bin);

    char signature_hex[65];
    to_hex(signature_bin, SHA256_DIGEST_LENGTH, signature_hex);

    /* Step 6: Authorization header */
    char *authorization_header = apr_psprintf(
        pool,
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
        creds->access_key, credential_scope, signed_headers, signature_hex);

    /* Step 7: Send request with libcurl */
    char *endpoint = apr_psprintf(pool, ENDPOINT_FMT, creds->region);
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to init curl\n");
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers =
        curl_slist_append(headers, "Content-Type: application/x-amz-json-1.0");
    headers = curl_slist_append(headers, apr_psprintf(pool, "Host: %s", host));
    headers = curl_slist_append(headers,
                                apr_psprintf(pool, "X-Amz-Date: %s", amz_date));
    headers = curl_slist_append(
        headers,
        apr_psprintf(pool, "X-Amz-Target: DynamoDB_20120810.%s", operation));
    headers = curl_slist_append(
        headers, apr_psprintf(pool, "Authorization: %s", authorization_header));

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? 0 : -1;
}
