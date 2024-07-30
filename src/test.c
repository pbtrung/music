#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <faad.h>
#include <samplerate.h>

#define BUFFER_SIZE 8192

// Function to decode and (optionally) resample to PCM file
void decode_to_pcm(const char *input_file_path, const char *output_file_path, bool resample) {
    FILE *input_file, *output_file;
    NeAACDecHandle decoder;
    NeAACDecFrameInfo frame_info;
    unsigned char input_buffer[BUFFER_SIZE];
    void *output_buffer;
    long input_size;
    unsigned long sample_rate, channels;

    // Open the input file
    input_file = fopen(input_file_path, "rb");
    if (!input_file) {
        perror("fopen (input)");
        exit(EXIT_FAILURE);
    }

    // Open the output file
    output_file = fopen(output_file_path, "wb");
    if (!output_file) {
        perror("fopen (output)");
        fclose(input_file);
        exit(EXIT_FAILURE);
    }

    // Initialize FAAD2 decoder
    decoder = NeAACDecOpen();
    if (decoder == NULL) {
        fprintf(stderr, "Failed to open FAAD2 decoder\n");
        goto cleanup;
    }

    // Read initial data to initialize decoder
    input_size = fread(input_buffer, 1, BUFFER_SIZE, input_file);
    if (NeAACDecInit(decoder, input_buffer, input_size, &sample_rate, &channels) < 0) {
        fprintf(stderr, "Failed to initialize FAAD2 decoder\n");
        goto cleanup;
    }

    SRC_STATE *resampler = NULL;
    if (resample) {
        // Initialize resampler (if needed)
        SRC_DATA resample_data;
        resample_data.data_in = NULL; // Will be set per frame
        resample_data.data_out = malloc(BUFFER_SIZE * 2); // Output buffer
        resample_data.input_frames = 0; // Will be set per frame
        resample_data.output_frames = BUFFER_SIZE / 2; // Max output samples
        resample_data.end_of_input = 0;
        int error = src_simple(&resample_data, SRC_SINC_BEST_QUALITY, channels);
        if (error != 0) {
            fprintf(stderr, "Failed to initialize resampler: %s\n", src_strerror(error));
            goto cleanup;
        }
        resampler = resample_data.src_state;
    }

    // Decode loop
    while (input_size > 0) {
        output_buffer = NeAACDecDecode(decoder, &frame_info, input_buffer, input_size);

        if (frame_info.error > 0) {
            fprintf(stderr, "Decoding error: %s\n", NeAACDecGetErrorMessage(frame_info.error));
            break;
        }

        if (frame_info.samples > 0) {
            if (!resample || (frame_info.samplerate == 48000 && frame_info.channels == 2)) {
                fwrite(output_buffer, 2, frame_info.samples, output_file);
            } else {
                SRC_DATA resample_data;
                resample_data.data_in = output_buffer;
                resample_data.data_out = malloc(BUFFER_SIZE * 2);
                resample_data.input_frames = frame_info.samples;
                resample_data.output_frames = BUFFER_SIZE / 2;
                resample_data.end_of_input = 0;
                resample_data.src_ratio = 48000.0 / frame_info.samplerate;

                int error = src_process(resampler, &resample_data);
                if (error != 0) {
                    fprintf(stderr, "Resampling error: %s\n", src_strerror(error));
                    break;
                } else {
                    fwrite(resample_data.data_out, 2, resample_data.output_frames_gen, output_file);
                }
            }
        }

        input_size = fread(input_buffer, 1, BUFFER_SIZE, input_file);
    }
cleanup:
    // Clean up
    fclose(input_file);
    fclose(output_file);
    NeAACDecClose(decoder);
    if (resampler) {
        src_delete(resampler);
    }
}

int main(int argc, char *argv[]) {
    bool resample = true;
    decode_to_pcm(argv[1], argv[2], resample);
    return 0;
}
