#ifndef FFMPEG_H
#define FFMPEG_H

void decode_audio(const char *input_filename, const char *output_pipe,
                  const char *ext);

#endif // FFMPEG_H
