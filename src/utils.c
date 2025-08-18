#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <apr_lib.h>
#include <apr_random.h>

#include "const.h"
#include "log.h"
#include "utils.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// Input validation functions
static bool validate_str(const char *str, size_t max_len) {
    if (!str) {
        log_trace("validate_str: NULL string");
        return false;
    }

    size_t len = strlen(str);
    if (len == 0 || len > max_len) {
        log_trace("validate_str: Invalid string length: %zu", len);
        return false;
    }

    return true;
}

static bool validate_path(const char *component) {
    if (!component)
        return false;

    // Check for directory traversal attempts
    if (strstr(component, "..") || strstr(component, "/") ||
        strstr(component, "\\")) {
        log_trace("validate_path: Invalid path component: %s", component);
        return false;
    }

    return true;
}

static bool validate_filename(const char *filename) {
    if (!validate_str(filename, MAX_FILENAME_LENGTH))
        return false;

    // Check for invalid filename characters
    const char *invalid_chars = "<>:\"|?*";
    for (const char *p = invalid_chars; *p; p++) {
        if (strchr(filename, *p)) {
            log_trace("validate_filename: Invalid character '%c' in filename",
                      *p);
            return false;
        }
    }

    return true;
}

void util_trim_spaces(char *str) {
    if (!str) {
        log_trace("util_trim_spaces: NULL string");
        return;
    }

    size_t length = strlen(str);
    if (length == 0)
        return;

    size_t i = 0, j = 0;
    bool space_found = false;

    while (i < length) {
        if (str[i] != ' ') {
            str[j++] = str[i++];
            space_found = false;
        } else {
            if (!space_found) {
                str[j++] = ' ';
                space_found = true;
            }
            i++;
        }
    }

    // Remove trailing space if present
    if (j > 0 && str[j - 1] == ' ')
        j--;

    // Null-terminate the string
    str[j] = '\0';
}

void util_format_time(int seconds, char *time_str, size_t time_str_size) {
    if (!time_str || time_str_size > MAX_TIME_STRING_LENGTH) {
        log_trace("util_format_time: Invalid parameters");
        return;
    }

    if (seconds < 0) {
        log_trace("util_format_time: Negative seconds value: %d", seconds);
        snprintf(time_str, time_str_size, "00:00");
        return;
    }

    // Prevent integer overflow in calculations
    if (seconds > INT_MAX / 3600) {
        log_trace("util_format_time: Seconds value too large: %d", seconds);
        snprintf(time_str, time_str_size, "99:59:59");
        return;
    }

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

char *util_make_path(char *output, char *filename) {
    if (!validate_str(output, MAX_PATH_LENGTH) ||
        !validate_filename(filename)) {
        log_trace("util_make_path: Invalid input parameters");
        return NULL;
    }

    // Additional validation for path components
    if (!validate_path(filename)) {
        log_trace("util_make_path: Invalid filename component");
        return NULL;
    }

    size_t output_len = strlen(output);
    size_t filename_len = strlen(filename);

    // Check for potential overflow: output + "/" + filename + null terminator
    if (output_len + filename_len + 2 > MAX_PATH_LENGTH) {
        log_trace("util_make_path: Path too long");
        return NULL;
    }

    size_t path_length = output_len + filename_len + 1; // +1 for "/"
    char *path = (char *)malloc(path_length + 1); // +1 for null terminator
    if (!path)
        util_error_exit("util_make_path: Memory allocation failed");

    snprintf(path, path_length + 1, "%s/%s", output, filename);
    return path;
}

void util_to_lower(char *str) {
    if (!str) {
        log_trace("util_to_lower: NULL string");
        return;
    }

    for (; *str; ++str)
        *str = apr_tolower((unsigned char)*str);
}

char *util_get_ext(const char *text) {
    if (!validate_str(text, MAX_FILENAME_LENGTH)) {
        log_trace("util_get_ext: Invalid input text");
        return NULL;
    }

    pcre2_code *re = NULL;
    pcre2_match_data *match_data = NULL;
    int errcode;
    PCRE2_SIZE erroffset;
    PCRE2_SIZE *ovector;
    size_t ext_len;
    char *ext = NULL;

    // Compile the regular expression for file extension
    re = pcre2_compile(
        (PCRE2_SPTR) "(.*)\\.(opus|mp3|m4a|m4b)$",
        PCRE2_ZERO_TERMINATED, // Pattern length (0 means null-terminated)
        PCRE2_CASELESS,        // Options
        &errcode,              // Error code
        &erroffset,            // Error offset
        NULL);                 // Compile context

    if (!re) {
        PCRE2_UCHAR buffer[256];
        pcre2_get_error_message(errcode, buffer, sizeof(buffer));
        log_trace("util_get_ext: PCRE2 compilation failed: %s", buffer);
        return NULL;
    }

    // Create a match data block
    match_data = pcre2_match_data_create_from_pattern(re, NULL);
    if (!match_data) {
        log_trace("util_get_ext: Failed to create match data for: %s", text);
        pcre2_code_free(re);
        return NULL;
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
        log_trace("util_get_ext: No match for: %s", text);
        goto cleanup;
    }

    // Extract the file extension from the match data
    ovector = pcre2_get_ovector_pointer(match_data);

    if (rc >= 3) {
        // Group 2 contains the file extension
        ext_len = ovector[5] - ovector[4];

        // Validate extension length
        if (ext_len == 0 || ext_len > MAX_EXTENSION_LENGTH) {
            log_trace("util_get_ext: Invalid extension length: %zu", ext_len);
            goto cleanup;
        }

        ext = (char *)malloc(ext_len + 1);
        if (!ext)
            util_error_exit("util_get_ext: Memory allocation failed");

        memcpy(ext, text + ovector[4], ext_len);
        ext[ext_len] = '\0'; // Null-terminate the string

        util_to_lower(ext);
    }

cleanup:
    if (re)
        pcre2_code_free(re);
    if (match_data)
        pcre2_match_data_free(match_data);
    return ext;
}

char *util_gen_filename(char *text) {
    if (!validate_str(text, MAX_FILENAME_LENGTH)) {
        log_trace("util_gen_filename: Invalid input text");
        return NULL;
    }

    char *ext = util_get_ext(text);
    if (!ext) {
        log_trace("util_gen_filename: Failed to get extension");
        return NULL;
    }

    char *fn = util_rand_str(DEFAULT_FILENAME_LENGTH);
    if (!fn) {
        log_trace("util_gen_filename: Failed to generate random filename");
        free(ext);
        return NULL;
    }

    size_t fn_len = strlen(fn);
    size_t ext_len = strlen(ext);
    size_t filename_len = fn_len + 1 + ext_len; // +1 for dot

    // Check for potential overflow
    if (filename_len > MAX_FILENAME_LENGTH) {
        log_trace("util_gen_filename: Filename too long");
        free(fn);
        free(ext);
        return NULL;
    }

    char *filename = (char *)malloc(filename_len + 1);
    if (!filename)
        util_error_exit("util_gen_filename: Memory allocation failed");

    snprintf(filename, filename_len + 1, "%s.%s", fn, ext);
    free(fn);
    free(ext);
    return filename;
}

int *util_rand_ints(int count, int min_val, int max_val) {
    // Input validation
    if (count <= 0) {
        log_trace("util_rand_ints: Invalid count: %d", count);
        return NULL;
    }

    if (min_val > max_val) {
        log_trace("util_rand_ints: Invalid range: min=%d, max=%d", min_val,
                  max_val);
        return NULL;
    }

    // Check for potential overflow in range calculation
    if (max_val > INT_MAX - 1 || min_val < INT_MIN) {
        log_trace("util_rand_ints: Range values too extreme");
        return NULL;
    }

    long long range = (long long)max_val - (long long)min_val + 1;
    if (count > range) {
        log_trace("util_rand_ints: Count (%d) exceeds range (%lld)", count,
                  range);
        return NULL;
    }

    // Check for potential overflow in allocation
    if (count > SIZE_MAX / sizeof(int)) {
        log_trace("util_rand_ints: Too many samples requested");
        return NULL;
    }

    int *unique_ints = malloc(count * sizeof(int));
    if (!unique_ints)
        util_error_exit("util_rand_ints: Memory allocation failed");

    int generated = 0;
    int max_attempts = count * 10; // Prevent infinite loops
    int attempts = 0;

    while (generated < count && attempts < max_attempts) {
        unsigned long random_byte;
        apr_status_t status = apr_generate_random_bytes(
            (unsigned char *)&random_byte, sizeof(random_byte));
        if (status != APR_SUCCESS)
            util_error_exit("util_rand_ints: Failed to generate random bytes");

        // Use modulo safely for range mapping
        int rand_int = (int)((random_byte % range) + min_val);
        bool exists = false;

        // Check for duplicates
        for (int i = 0; i < generated; ++i) {
            if (unique_ints[i] == rand_int) {
                exists = true;
                break;
            }
        }

        if (!exists)
            unique_ints[generated++] = rand_int;

        attempts++;
    }

    // If we couldn't generate enough unique numbers
    if (generated < count)
        util_error_exit("util_rand_ints: Failed to generate unique numbers");

    return unique_ints;
}

char *util_rand_str(int length) {
    // Input validation
    if (length < MIN_RANDOM_STRING_LENGTH ||
        length > MAX_RANDOM_STRING_LENGTH) {
        log_trace("util_rand_str: Invalid length: %d", length);
        return NULL;
    }

    const char *alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int alphabet_size = strlen(alphabet);

    // Check for potential overflow in allocation
    if (length > SIZE_MAX - 1) {
        log_trace("util_rand_str: Length too large");
        return NULL;
    }

    char *result = malloc((length + 1) * sizeof(char));
    if (!result)
        util_error_exit("util_rand_str: Memory allocation failed");

    for (int i = 0; i < length; ++i) {
        unsigned long random_byte;
        apr_status_t status = apr_generate_random_bytes(
            (unsigned char *)&random_byte, sizeof(random_byte));
        if (status != APR_SUCCESS)
            util_error_exit("util_rand_str: Failed to generate random bytes");

        result[i] = alphabet[random_byte % alphabet_size];
    }
    result[length] = '\0';

    return result;
}

char *util_format_commas(long num, char *buf, size_t bufsize) {
    if (!buf || bufsize == 0) {
        log_trace("util_format_commas: Invalid buffer parameters");
        return NULL;
    }

    char tmp[64];
    int ret = snprintf(tmp, sizeof(tmp), "%ld", num);

    // Check for snprintf errors
    if (ret < 0 || ret >= (int)sizeof(tmp)) {
        log_trace("util_format_commas: Number formatting failed");
        snprintf(buf, bufsize, "0");
        return buf;
    }

    int len = ret;
    int commas = (len - 1) / 3;

    // Check if we have enough space in the output buffer
    if ((size_t)(len + commas + 1) > bufsize) {
        // Not enough space, return plain number
        snprintf(buf, bufsize, "%ld", num);
        return buf;
    }

    int i = len - 1;
    int j = len + commas;
    buf[j--] = '\0';

    int count = 0;
    while (i >= 0 && j >= 0) {
        buf[j--] = tmp[i--];
        if (++count == 3 && i >= 0 && j >= 0) {
            buf[j--] = ',';
            count = 0;
        }
    }

    return buf;
}

char *util_safe_strdup(const char *str, const char *context) {
    if (!str) {
        log_trace("safe_strdup: NULL string in %s", context);
        return NULL;
    }

    size_t len = strlen(str);
    if (len > MAX_TASK_FIELD_LENGTH) {
        log_trace("safe_strdup: String too long in %s: %zu", context, len);
        return NULL;
    }

    char *dup = malloc(len + 1);
    if (!dup) {
        log_trace("safe_strdup: Memory allocation failed in %s", context);
        return NULL;
    }

    strcpy(dup, str);
    return dup;
}

void util_error_exit(const char *msg) {
    if (msg) {
        fprintf(stderr, "Error: %s\n", msg);
        log_trace("Error: %s", msg);
    }
    exit(-1);
}
