#include "util.h"
#include <apr_lib.h>
#include <apr_random.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

void util_remove_spaces(char *str) {
    int i = 0, j = 0;
    int length = strlen(str);
    int spaceFound = 0;

    while (i < length) {
        if (str[i] != ' ') {
            str[j++] = str[i++];
            spaceFound = 0;
        } else {
            if (!spaceFound) {
                str[j++] = ' ';
                spaceFound = 1;
            }
            i++;
        }
    }

    // Remove trailing space if present
    if (j > 0 && str[j - 1] == ' ') {
        j--;
    }

    // Null-terminate the string
    str[j] = '\0';
}

void util_seconds_to_time(int seconds, char *time_str, size_t time_str_size) {
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int remaining_seconds = seconds % 60;

    if (hours > 0) {
        snprintf(time_str, time_str_size, "%02d:%02d:%02d", hours, minutes,
                 remaining_seconds);
    } else {
        snprintf(time_str, time_str_size, "%02d:%02d", minutes,
                 remaining_seconds);
    }
}

char *util_get_file_path(char *output, char *filename) {
    int path_length = strlen(output) + strlen(filename) + 1;
    char *path = (char *)malloc(path_length + 1);
    if (!path) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    snprintf(path, path_length + 1, "%s/%s", output, filename);
    return path;
}

void util_tolower(char *str) {
    for (; *str; ++str)
        *str = apr_tolower((unsigned char)*str);
}

char *util_get_extension(const char *text) {
    pcre2_code *re;
    pcre2_match_data *match_data;
    int errcode;
    PCRE2_SIZE erroffset;
    PCRE2_SIZE *ovector;
    size_t ext_len;
    char *ext = NULL;

    // Compile the regular expression for file extension
    re = pcre2_compile(
        (PCRE2_SPTR) "(.*)\\.(opus|mp3|m4a)$",
        PCRE2_ZERO_TERMINATED, // Pattern length (0 means null-terminated)
        PCRE2_CASELESS,        // Options
        &errcode,              // Error code
        &erroffset,            // Error offset
        NULL);                 // Compile context

    if (!re) {
        fprintf(stderr, "PCRE2 compilation failed\n");
        exit(-1);
    }

    // Create a match data block
    match_data = pcre2_match_data_create_from_pattern(re, NULL);
    if (!match_data) {
        fprintf(stderr, "Failed to create match data\n");
        exit(-1);
    }

    // Perform the match
    int rc = pcre2_match(re,               // Compiled pattern
                         (PCRE2_SPTR)text, // Subject string
                         strlen(text),     // Length of subject string
                         0,                // Starting offset
                         0,                // Options
                         match_data,       // Match data
                         NULL              // Match context
    );

    if (rc < 0) {
        fprintf(stderr, "No match\n");
        exit(-1);
    }

    // Extract the file extension from the match data
    ovector = pcre2_get_ovector_pointer(match_data);

    if (rc >= 3) {
        // Group 2 contains the file extension
        ext_len = ovector[5] - ovector[4];
        ext = (char *)malloc(ext_len + 1);
        if (!ext) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
        memcpy(ext, text + ovector[4], ext_len);
        ext[ext_len] = '\0'; // Null-terminate the string
    }

    util_tolower(ext);
    pcre2_code_free(re);
    pcre2_match_data_free(match_data);

    return ext;
}

char *util_get_filename_with_extension(char *text) {
    char *ext = util_get_extension(text);
    int fn_len = 20;
    char *fn = util_random_string(fn_len);
    int filename_len = fn_len + 1 + strlen(ext);
    char *filename = (char *)malloc(filename_len + 1);
    if (!filename) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    snprintf(filename, filename_len + 1, "%s.%s", fn, ext);
    free(fn);
    free(ext);
    return filename;
}

int *util_random_ints(int num_samples, int min_value, int max_value) {
    if (num_samples > (max_value - min_value + 1)) {
        fprintf(stderr,
                "Number of samples exceeds the range of unique values\n");
        return NULL;
    }

    int *unique_random_ints = malloc(num_samples * sizeof(int));
    if (unique_random_ints == NULL) {
        perror("Failed to allocate memory");
        exit(-1);
    }

    int count = 0;
    while (count < num_samples) {
        unsigned long random_byte;
        apr_generate_random_bytes((unsigned char *)&random_byte,
                                  sizeof(random_byte));
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

char *util_random_string(int length) {
    const char *alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int alphabet_size = strlen(alphabet);

    char *result = malloc((length + 1) * sizeof(char));
    if (result == NULL) {
        perror("Failed to allocate memory");
        exit(-1);
    }

    for (int i = 0; i < length; ++i) {
        unsigned long random_byte;
        apr_generate_random_bytes((unsigned char *)&random_byte,
                                  sizeof(random_byte));
        result[i] = alphabet[random_byte % alphabet_size];
    }
    result[length] = '\0';

    return result;
}