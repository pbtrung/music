#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 4096

// Structure to store PCM audio data
typedef struct {
    short *data;
    size_t size; // Size in bytes
} PCMData;

PCMData decode_mp3(const char *filename) {
    PCMData pcmData = {NULL, 0};
    mpg123_handle *mh = NULL;
    int err;
    unsigned char *audio = NULL;
    size_t done;
    size_t data_size = 0;

    // Initialize mpg123 library
    if (mpg123_init() != MPG123_OK) {
        fprintf(stderr, "Unable to initialize mpg123 library\n");
        return pcmData;
    }

    // Create a new mpg123 handle
    mh = mpg123_new(NULL, &err);
    if (!mh) {
        fprintf(stderr, "Error creating mpg123 handle: %s\n",
                mpg123_plain_strerror(err));
        mpg123_exit();
        return pcmData;
    }

    // Open the MP3 file
    if (mpg123_open(mh, filename) != MPG123_OK) {
        fprintf(stderr, "Error opening file: %s\n", filename);
        mpg123_delete(mh);
        mpg123_exit();
        return pcmData;
    }

    // Get the audio format of the MP3 file
    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        fprintf(stderr, "Error getting MP3 format: %s\n", mpg123_strerror(mh));
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return pcmData;
    }
    printf("MP3 File format: Rate=%ld, Channels=%d, Encoding=%d\n", rate,
           channels, encoding);

    // Force output format to 48kHz, 16-bit stereo (if possible)
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, 48000, 2, MPG123_ENC_SIGNED_16) != MPG123_OK) {
        fprintf(stderr, "Error setting format: %s\n", mpg123_strerror(mh));
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return pcmData;
    }

    // Allocate buffer for audio data
    audio = (unsigned char *)malloc(BUFFER_SIZE);
    if (!audio) {
        fprintf(stderr, "Error allocating audio buffer\n");
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
        return pcmData;
    }

    // Read and decode MP3 data
    while ((err = mpg123_read(mh, audio, BUFFER_SIZE, &done)) == MPG123_OK) {
        size_t new_size = data_size + done;
        short *new_data = (short *)realloc(pcmData.data, new_size);
        if (!new_data) {
            fprintf(stderr, "Error reallocating PCM data buffer\n");
            free(audio);
            free(pcmData.data);
            mpg123_close(mh);
            mpg123_delete(mh);
            mpg123_exit();
            return pcmData;
        }
        pcmData.data = new_data;
        memcpy((unsigned char *)pcmData.data + data_size, audio, done);
        data_size = new_size;
    }

    // Check for errors during decoding
    if (err != MPG123_DONE) {
        fprintf(stderr, "Error decoding MP3 file: %s\n", mpg123_strerror(mh));
        free(pcmData.data); // Free the data in case of an error
        pcmData.data = NULL;
        pcmData.size = 0;
    } else {
        pcmData.size = data_size;
    }

    // Clean up
    free(audio);
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    return pcmData;
}

void writePCMDataToFile(const PCMData *pcmData, const char *filename) {
    FILE *file = fopen(filename, "wb"); // Open file in binary mode for writing
    if (!file) {
        perror("Error opening PCM output file");
        return;
    }

    // Write the PCM data to the file
    if (fwrite(pcmData->data, 1, pcmData->size, file) != pcmData->size) {
        fprintf(stderr, "Error writing PCM data to file\n");
    }

    fclose(file);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_mp3> <output_pcm>\n", argv[0]);
        return EXIT_FAILURE;
    }

    PCMData pcmData = decode_mp3(argv[1]);
    if (pcmData.data) {
        writePCMDataToFile(&pcmData, argv[2]);
        free(pcmData.data);
    } else {
        fprintf(stderr, "No PCM data available\n");
    }

    return EXIT_SUCCESS;
}
