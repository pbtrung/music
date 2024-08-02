#include "utils.h"
#include "websocket.h"
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <mpg123.h>
#include <opusfile.h>
#include <unistd.h>

static void decode_mp3(const char *filename, const char *pipe_name) {
    mpg123_handle *mh = NULL;
    int err;
    unsigned char *audio = NULL;
    size_t done;
    int buffer_size = 4096;

    // Initialize mpg123 library
    if (mpg123_init() != MPG123_OK) {
        fprintf(stderr, "Unable to initialize mpg123 library\n");
        return;
    }

    // Create a new mpg123 handle
    mh = mpg123_new(NULL, &err);
    if (!mh) {
        fprintf(stderr, "Error creating mpg123 handle\n");
        goto cleanup;
    }

    // Open the MP3 file
    if (mpg123_open(mh, filename) != MPG123_OK) {
        fprintf(stderr, "Error opening file: %s\n", filename);
        goto cleanup;
    }

    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        fprintf(stderr, "Error getting MP3 format\n");
        goto cleanup;
    }

    channels = 2;
    encoding = MPG123_ENC_SIGNED_16;
    rate = 48000;
    // Ensure the output format is correct
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, rate, channels, encoding) != MPG123_OK) {
        fprintf(stderr, "Error setting format\n");
        goto cleanup;
    }

    // Open the named pipe for writing
    int fd = open(pipe_name, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening pipe\n");
        goto cleanup;
    }

    // Allocate buffer for audio data
    audio = (unsigned char *)malloc(buffer_size);
    if (!audio) {
        fprintf(stderr, "Error allocating audio buffer\n");
        goto cleanup;
    }

    // Read and decode MP3 data
    while ((err = mpg123_read(mh, audio, buffer_size, &done)) == MPG123_OK) {
        if (write(fd, audio, done) == -1) {
            fprintf(stderr, "Error writing to pipe\n");
            goto cleanup;
        }
    }

    if (err != MPG123_DONE) {
        fprintf(stderr, "Error decoding MP3 file\n");
    }

cleanup:
    free(audio);
    if (fd >= 0)
        close(fd);
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
}

static void decode_opus(const char *filename, const char *pipe_name) {
    int error;
    int channels = 2;
    int bits_per_sample = 16;
    int buffer_size = 4096;

    OggOpusFile *of = op_open_file(filename, &error);
    if (!of) {
        fprintf(stderr, "Error opening file: %s\n", filename);
        goto cleanup;
    }

    int fd = open(pipe_name, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening pipe\n");
        goto cleanup;
    }

    // Buffer for PCM data
    opus_int16 *pcm =
        (opus_int16 *)malloc(buffer_size * channels * sizeof(opus_int16));
    if (!pcm) {
        fprintf(stderr, "Error allocating PCM buffer\n");
        goto cleanup;
    }

    int ret;
    while ((ret = op_read_stereo(of, pcm, buffer_size)) > 0) {
        ssize_t written =
            write(fd, pcm, ret * channels * (bits_per_sample / 8));
        if (written == -1) {
            fprintf(stderr, "Error writing to pipe\n");
            goto cleanup;
        }
    }

    if (ret < 0) {
        fprintf(stderr, "Error decoding Opus file\n");
    }

cleanup:
    free(pcm);
    if (of)
        op_free(of);
    if (fd >= 0)
        close(fd);
}

static void print_metadata(config_t *config, AVFormatContext *fmt_ctx) {
    AVDictionaryEntry *tag = NULL;

    const int width = 17;
    // Print format-level metadata
    while ((
        tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        to_lowercase(tag->key);
        print_kv(config, width, tag->key, tag->value);
    }
    // Print stream-level metadata
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            to_lowercase(tag->key);
            print_kv(config, width, tag->key, tag->value);
        }
    }
}

static void print_duration(config_t *config, AVFormatContext *fmt_ctx) {
    const int width = 17;
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        // Convert duration from microseconds to seconds
        double duration_seconds = fmt_ctx->duration / (double)AV_TIME_BASE;
        char msg[128];
        int msg_len = snprintf(msg, 128, "%-*s: %.2f\n", width, "duration",
                               duration_seconds);
        printf("%s", msg);
        write_message(config, msg, msg_len);
    } else {
        print_kv(config, width, "duration", "Unknown");
    }
}

static void decode_others(const char *input_filename, const char *output_pipe) {
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    const AVCodec *codec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr_ctx = NULL;
    FILE *output_fp = NULL;
    int stream_index = -1;
    int ret;

    av_log_set_level(AV_LOG_ERROR);

    if ((ret = avformat_open_input(&fmt_ctx, input_filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open source file %s\n", input_filename);
        goto cleanup;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto cleanup;
    }

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = i;
            break;
        }
    }

    if (stream_index == -1) {
        fprintf(stderr, "Could not find audio stream\n");
        goto cleanup;
    }

    codec = avcodec_find_decoder(
        fmt_ctx->streams[stream_index]->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Failed to find codec\n");
        goto cleanup;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec\n");
        goto cleanup;
    }

    if ((ret = avcodec_parameters_to_context(
             codec_ctx, fmt_ctx->streams[stream_index]->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        goto cleanup;
    }

    if ((ret = avcodec_open2(codec_ctx, codec, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        goto cleanup;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        goto cleanup;
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate AVFrame\n");
        goto cleanup;
    }

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate SwrContext\n");
        goto cleanup;
    }

    av_opt_set_chlayout(swr_ctx, "in_chlayout", &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);

    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, 2);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        goto cleanup;
    }

    output_fp = fopen(output_pipe, "wb");
    if (!output_fp) {
        fprintf(stderr, "Could not open output pipe %s\n", output_pipe);
        goto cleanup;
    }

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_index) {
            ret = avcodec_send_packet(codec_ctx, pkt);
            if (ret < 0) {
                fprintf(stderr, "Error submitting the packet to the decoder\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "Error during decoding\n");
                    goto cleanup;
                }

                uint8_t *output_buffer = NULL;
                int output_buffer_size =
                    av_samples_alloc(&output_buffer, NULL, 2, frame->nb_samples,
                                     AV_SAMPLE_FMT_S16, 0);
                if (output_buffer_size < 0) {
                    fprintf(stderr, "Could not allocate output buffer\n");
                    goto cleanup;
                }

                int nb_samples = swr_convert(
                    swr_ctx, &output_buffer, frame->nb_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);
                if (nb_samples < 0) {
                    fprintf(stderr, "Error while converting\n");
                    av_freep(&output_buffer);
                    goto cleanup;
                }

                fwrite(output_buffer, 1,
                       nb_samples * 2 *
                           av_get_bytes_per_sample(AV_SAMPLE_FMT_S16),
                       output_fp);
                av_freep(&output_buffer);
            }
        }
        av_packet_unref(pkt);
    }

cleanup:
    if (output_fp)
        fclose(output_fp);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    swr_free(&swr_ctx);
}

void decode_audio(config_t *config, const char *input_filename,
                  const char *ext) {
    int ret;
    AVFormatContext *fmt_ctx = NULL;

    av_log_set_level(AV_LOG_ERROR);

    if ((ret = avformat_open_input(&fmt_ctx, input_filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open source file %s\n", input_filename);
        goto cleanup;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto cleanup;
    }

    print_metadata(config, fmt_ctx);
    print_duration(config, fmt_ctx);
    fflush(stdout);

    if (strcmp(ext, "mp3") == 0) {
        decode_mp3(input_filename, config->pipe_name);
    } else if (strcmp(ext, "opus") == 0) {
        decode_opus(input_filename, config->pipe_name);
    } else {
        decode_others(input_filename, config->pipe_name);
    }

cleanup:
    avformat_close_input(&fmt_ctx);
}
