#ifndef UTILS_H
#define UTILS_H

void to_lowercase(char *str);
char *get_extension(const char *text);
char *get_filename_with_ext(char *text);
char *get_file_path(char *output, char *filename);
char *int_to_str(int num);

#endif // UTILS_H
