#ifndef UTILS_H
#define UTILS_H

#include "config.h"

void to_lowercase(char *str);
char *get_extension(const char *text);
char *get_filename_with_ext(char *text);
char *get_file_path(char *output, char *filename);
char *int_to_str(int num);
void print_kv(config_t *config, int width, char *key, char *value);
void print_end(config_t *config);

#endif // UTILS_H
