#ifndef AWS_DYNAMODB_H
#define AWS_DYNAMODB_H

#include <apr_pools.h>
#include <curl/curl.h>

typedef struct {
    const char *access_key;
    const char *secret_key;
    const char *region;
} aws_credentials_t;

int aws_dynamodb_init(apr_pool_t **pool);
void aws_dynamodb_cleanup(apr_pool_t *pool);
int aws_dynamodb_request(apr_pool_t *pool, const aws_credentials_t *creds,
                         const char *operation, const char *payload);

#endif // AWS_DYNAMODB_H
