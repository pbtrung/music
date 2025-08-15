#ifndef UTILS_H
#define UTILS_H

#include "config.h"

void util_to_lower(char *str);
char *util_get_ext(const char *text);
char *util_gen_filename(char *text);
char *util_make_path(char *output, char *filename);
int *util_rand_ints(int num_samples, int min_value, int max_value);
char *util_rand_str(int length);
void util_format_time(int seconds, char *time_str, size_t time_str_size);
void util_trim_spaces(char *str);
char *util_format_commas(long num, char *buf, size_t bufsize);

#endif // UTILS_H