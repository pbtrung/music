#include "track.h"
#include "downloader.h"
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <mpg123.h>
#include <opusfile.h>
#include <string.h>
#include <sys/stat.h>
#include <tag_c.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

void decode_mp3(const char *filename, const char *pipe_name) {
    mpg123_handle *mh;
    int err, channels, encoding;
    long rate;
    unsigned char *audio;
    size_t done;

    // Initialize mpg123 library
    mpg123_init();
    mh = mpg123_new(NULL, &err);
    if (!mh) {
        fprintf(stderr, "Error creating mpg123 handle\n");
        exit(-1);
    }

    // Open MP3 file
    if (mpg123_open(mh, filename) != MPG123_OK) {
        fprintf(stderr, "Error opening file\n");
        exit(-1);
    }

    // Set format (48000 Hz, 16-bit, stereo)
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, 48000, MPG123_STEREO, MPG123_ENC_SIGNED_16) !=
            MPG123_OK) {
        fprintf(stderr, "Error setting format\n");
        exit(-1);
    }

    // Open pipe
    int fd = open(pipe_name, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening pipe\n");
        exit(-1);
    }

    // Decode and write to pipe
    while (mpg123_read(mh, &audio, BUFFER_SIZE, &done) == MPG123_OK) {
        write(fd, audio, done);
    }

    mpg123_close(mh);
    mpg123_delete(mh);
    close(fd);
    mpg123_exit();
}

void decode_opus(const char *filename, const char *pipe_name) {
    int error;
    int channels = 2;
    int bits_per_sample = 16;
    int sample_rate = 48000;

    OggOpusFile *of = op_open_file(filename, &error);
    if (!of) {
        fprintf(stderr, "Error opening file\n");
        exit(-1);
    }

    int fd = open(pipe_name, O_WRONLY);
    if (fd == -1) {
        perror("Error opening pipe");
        exit(-1);
    }

    // Set up decoder
    OpusDecoder *dec = opus_decoder_create(sample_rate, channels, &error);
    if (error != OPUS_OK) {
        fprintf(stderr, "Error creating Opus decoder\n");
        exit(-1);
    }

    opus_int16 pcm[BUFFER_SIZE * channels];
    int ret;
    while ((ret = op_read_stereo(of, pcm, BUFFER_SIZE)) > 0) {
        unsigned char output[BUFFER_SIZE * channels * (bits_per_sample / 8)];
        int decoded_samples = opus_decode(dec, (unsigned char *)pcm, ret, (opus_int16 *)output, BUFFER_SIZE, 0);
         if (decoded_samples < 0) {
            fprintf(stderr, "Opus decode failed\n");
            exit(-1);
        }
        write(fd, output, ret * channels * (bits_per_sample / 8));
    }

    op_free(of);
    opus_decoder_destroy(dec);
    close(fd);
}

void track_decode(file_downloader_t *infos, int num_files) {
    for (int i = 0; i < num_files; ++i) {
        int path_length =
            strlen(infos[i].config->output) + strlen(infos[i].filename) + 2;
        char *path = (char *)malloc(path_length);
        snprintf(path, path_length, "%s/%s", infos[i].config->output,
                 infos[i].filename);

        fprintf(stdout, "Decoding: %s\n", path);
        if (strcmp(infos[i].ext, "mp3") == 0) {
            decode_mp3(path, infos[i].config->pipe_name);
        } else if (strcmp(infos[i].ext, "opus") == 0) {
            decode_opus(path, infos[i].config->pipe_name);
        }
        free(path);
    }
}

char *track_extract_metadata(file_downloader_t *infos, int num_files) {
    json_t *metadata = json_array();

    for (int i = 0; i < num_files; ++i) {
        int path_length =
            strlen(infos[i].config->output) + strlen(infos[i].filename) + 2;
        char *path = (char *)malloc(path_length);
        snprintf(path, path_length, "%s/%s", infos[i].config->output,
                 infos[i].filename);

        json_t *mdata = json_object();

        if (strcmp(infos[i].ext, "mp3") == 0 ||
            strcmp(infos[i].ext, "opus") == 0 ||
            strcmp(infos[i].ext, "m4a") == 0) {

            TagLib_File *file = taglib_file_new(path);
            if (file) {
                TagLib_Tag *tag = taglib_file_tag(file);

                json_object_set_new(mdata, "artist",
                                    json_string(taglib_tag_artist(tag)
                                                    ? taglib_tag_artist(tag)
                                                    : "Unknown"));
                json_object_set_new(mdata, "album",
                                    json_string(taglib_tag_album(tag)
                                                    ? taglib_tag_album(tag)
                                                    : infos[i].album_path));
                json_object_set_new(mdata, "title",
                                    json_string(taglib_tag_title(tag)
                                                    ? taglib_tag_title(tag)
                                                    : infos[i].track_name));

                taglib_file_free(file);
            } else {
                fprintf(stderr, "Error opening file: %s\n", path);
                json_decref(mdata);
            }
        } else {
            fprintf(stderr, "Unsupported file format: %s\n", infos[i].ext);
            json_decref(mdata);
        }

        if (json_object_size(mdata) > 0) {
            json_array_append_new(metadata, mdata);
        }
        free(path);
    }

    json_t *root = json_object();
    json_object_set_new(root, "metadata", metadata);

    char *json_str = json_dumps(root, 0);
    json_decref(root);

    return json_str;
}
