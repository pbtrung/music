#include "utils.h"
#include "random.h"
#include "websocket.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

void print_kv(config_t *config, int width, char *key, char *value) {
    int msg_len = 1024;
    char *msg = (char *)malloc(msg_len);
    if (!msg) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    msg_len = snprintf(msg, msg_len, "%-*s: %s\n", width, key, value);
    printf("%s", msg);
    write_message(config, msg, msg_len);
    free(msg);
}

void print_end(config_t *config) {
    int msg_len = 128;
    char *msg = (char *)malloc(msg_len);
    if (!msg) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    msg_len = snprintf(msg, msg_len, "%s", "\nend-5z2ok9v4iik5tdykgms90qrc6\n");
    printf("%s", msg);
    write_message(config, msg, msg_len);
    free(msg);
}

char *int_to_str(int num) {
    int max_len = snprintf(NULL, 0, "%d", num) + 1;
    char *str = (char *)malloc(max_len);
    if (!str) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    snprintf(str, max_len, "%d", num);
    return str;
}

char *get_file_path(char *output, char *filename) {
    int path_length = strlen(output) + strlen(filename) + 1;
    char *path = (char *)malloc(path_length + 1);
    if (!path) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(-1);
    }
    snprintf(path, path_length + 1, "%s/%s", output, filename);
    return path;
}

void to_lowercase(char *str) {
    for (; *str; ++str)
        *str = tolower((unsigned char)*str);
}

char *get_extension(const char *text) {
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

    to_lowercase(ext);
    pcre2_code_free(re);
    pcre2_match_data_free(match_data);

    return ext;
}

char *get_filename_with_ext(char *text) {
    char *ext = get_extension(text);
    int fn_len = 20;
    char *fn = random_string(fn_len);
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