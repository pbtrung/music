#ifndef UTIL_H
#define UTIL_H

#include "config.h"

void util_tolower(char *str);
char *util_get_extension(const char *text);
char *util_get_filename_with_extension(char *text);
char *util_get_file_path(char *output, char *filename);
int *util_random_ints(int num_samples, int min_value, int max_value);
char *util_random_string(int length);
void util_seconds_to_time(int seconds, char *time_str, size_t time_str_size);
void util_remove_spaces(char *str);

#endif // UTIL_H
