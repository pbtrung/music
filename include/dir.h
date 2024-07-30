#ifndef DIR_H
#define DIR_H

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int delete_directory(const char *path);
void create_directory(const char *path);

#endif // DIR_H
