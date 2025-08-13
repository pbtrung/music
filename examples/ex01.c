#include <stdio.h>

#include "aws_dynamodb.h"

int main() {
    apr_pool_t *pool;
    if (aws_dynamodb_init(&pool) != 0) return 1;

    aws_credentials_t creds = {
        .access_key = "YOUR_ACCESS_KEY",
        .secret_key = "YOUR_SECRET_KEY",
        .region     = "us-east-1"
    };

    const char *payload =
        "{\"TableName\":\"TestTable\",\"Key\":{\"id\":{\"S\":\"123\"}}}";

    if (aws_dynamodb_request(pool, &creds, "GetItem", payload) != 0) {
        fprintf(stderr, "Request failed\n");
    }

    aws_dynamodb_cleanup(pool);
    return 0;
}
