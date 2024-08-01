#include <ALACDecoder.h>
#include <sndfile.h> // For handling PCM files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 8192
#define OUTPUT_SAMPLE_RATE 48000
#define OUTPUT_CHANNELS 2
#define OUTPUT_SAMPLE_FMT SF_FORMAT_WAV | SF_FORMAT_PCM_16

void decode_alac_to_pcm(const char *input_file_path,
                        const char *output_file_path) {
    ALACDecoder *decoder;
    FILE *input_file;
    SNDFILE *output_file;
    SF_INFO sfinfo;
    unsigned char input_buffer[BUFFER_SIZE];
    int16_t output_buffer[BUFFER_SIZE * 2]; // Adjust size as needed
    size_t bytes_read;
    int num_samples;

    // Initialize ALAC decoder
    decoder = alac_decoder_init();
    if (!decoder) {
        fprintf(stderr, "Failed to initialize ALAC decoder\n");
        exit(EXIT_FAILURE);
    }

    // Open the input ALAC file
    input_file = fopen(input_file_path, "rb");
    if (!input_file) {
        perror("fopen");
        alac_decoder_free(decoder);
        exit(EXIT_FAILURE);
    }

    // Set up output file
    memset(&sfinfo, 0, sizeof(sfinfo));
    sfinfo.samplerate = OUTPUT_SAMPLE_RATE;
    sfinfo.channels = OUTPUT_CHANNELS;
    sfinfo.format = OUTPUT_SAMPLE_FMT;
    output_file = sf_open(output_file_path, SFM_WRITE, &sfinfo);
    if (!output_file) {
        fprintf(stderr, "Failed to open output file\n");
        fclose(input_file);
        alac_decoder_free(decoder);
        exit(EXIT_FAILURE);
    }

    // Decode loop
    while ((bytes_read = fread(input_buffer, 1, BUFFER_SIZE, input_file)) > 0) {
        num_samples = alac_decode(decoder, input_buffer, bytes_read,
                                  output_buffer, BUFFER_SIZE * 2);
        if (num_samples < 0) {
            fprintf(stderr, "Decoding error\n");
            break;
        }

        // Write decoded PCM data to output file
        sf_write_short(output_file, output_buffer, num_samples);
    }

    // Clean up
    fclose(input_file);
    sf_close(output_file);
    alac_decoder_free(decoder);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input_file> <output_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    decode_alac_to_pcm(argv[1], argv[2]);
    return EXIT_SUCCESS;
}
