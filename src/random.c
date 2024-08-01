#include "random.h"
#include <sodium.h>
#include <stdbool.h>
#include <string.h>


int *random_ints(int num_samples, int min_value, int max_value) {
    if (num_samples > (max_value - min_value + 1)) {
        fprintf(stderr,
                "Number of samples exceeds the range of unique values\n");
        return NULL;
    }

    int *unique_random_ints = malloc(num_samples * sizeof(int));
    if (unique_random_ints == NULL) {
        perror("Failed to allocate memory");
        exit(1);
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        exit(1);
    }

    int count = 0;
    while (count < num_samples) {
        unsigned long random_byte;
        randombytes_buf(&random_byte, sizeof(random_byte));
        int rand_int = (random_byte % (max_value - min_value + 1)) + min_value;
        bool exists = false;

        for (int i = 0; i < count; ++i) {
            if (unique_random_ints[i] == rand_int) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            unique_random_ints[count++] = rand_int;
        }
    }

    return unique_random_ints;
}

char *random_string(int length) {
    const char *alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int alphabet_size = strlen(alphabet);

    char *result = malloc((length + 1) * sizeof(char));
    if (result == NULL) {
        perror("Failed to allocate memory");
        exit(1);
    }

    if (sodium_init() < 0) {
        fprintf(stderr, "Failed to initialize libsodium\n");
        exit(1);
    }

    for (int i = 0; i < length; ++i) {
        unsigned long random_byte;
        randombytes_buf(&random_byte, sizeof(random_byte));
        result[i] = alphabet[random_byte % alphabet_size];
    }
    result[length] = '\0';

    return result;
}