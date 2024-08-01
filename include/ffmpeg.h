#ifndef FFMPEG_H
#define FFMPEG_H

#include "config.h"

void decode_audio(const char *input_filename, const char *output_pipe,
                  char *ext);

#endif // FFMPEG_H
