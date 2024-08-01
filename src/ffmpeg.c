#include "utils.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/log.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <stdlib.h>


static void print_metadata(AVFormatContext *fmt_ctx) {
    AVDictionaryEntry *tag = NULL;

    // Print format-level metadata
    while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        to_lowercase(tag->key);
        printf("%-17s: %s\n", tag->key, tag->value);
    }
    // Print stream-level metadata
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            to_lowercase(tag->key);
            printf("%-17s: %s\n", tag->key, tag->value);
        }
    }
}

static void print_duration(AVFormatContext *fmt_ctx) {
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        // Convert duration from microseconds to seconds
        double duration_seconds = fmt_ctx->duration / (double)AV_TIME_BASE;
        printf("%-17s: %.2f\n", "duration", duration_seconds);
    } else {
        printf("%-17s: %s\n", "duration", "Unknown");
    }
}

void decode_audio(const char *input_filename, const char *output_pipe,
                  char *ext) {
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
        exit(-1);
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(-1);
    }

    print_metadata(fmt_ctx);
    print_duration(fmt_ctx);
    fflush(stdout);

    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index = i;
            break;
        }
    }

    if (stream_index == -1) {
        fprintf(stderr, "Could not find audio stream\n");
        exit(-1);
    }

    codec = avcodec_find_decoder(
        fmt_ctx->streams[stream_index]->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Failed to find codec\n");
        exit(-1);
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec\n");
        exit(-1);
    }

    if ((ret = avcodec_parameters_to_context(
             codec_ctx, fmt_ctx->streams[stream_index]->codecpar)) < 0) {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        exit(-1);
    }

    if ((ret = avcodec_open2(codec_ctx, codec, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        exit(-1);
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(-1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate AVFrame\n");
        exit(-1);
    }

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate SwrContext\n");
        exit(-1);
    }

    av_opt_set_chlayout(swr_ctx, "in_chlayout", &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_channel_count",
                   codec_ctx->ch_layout.nb_channels, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);

    av_opt_set_chlayout(swr_ctx, "out_chlayout", &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_channel_count", 2, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        exit(-1);
    }

    output_fp = fopen(output_pipe, "wb");
    if (!output_fp) {
        fprintf(stderr, "Could not open output pipe %s\n", output_pipe);
        exit(-1);
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
                    exit(-1);
                }

                uint8_t *output_buffer;
                int output_buffer_size =
                    av_samples_alloc(&output_buffer, NULL, 2, frame->nb_samples,
                                     AV_SAMPLE_FMT_S16, 0);

                int nb_samples = swr_convert(
                    swr_ctx, &output_buffer, frame->nb_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);
                if (nb_samples < 0) {
                    fprintf(stderr, "Error while converting\n");
                    exit(-1);
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

    fclose(output_fp);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    swr_free(&swr_ctx);
}