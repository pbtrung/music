#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <stdlib.h>
#include <tag_c.h>


void taglib_print_metadata(const char *input_filename) {
    TagLib_File *file = taglib_file_new(input_filename);
    if (!file) {
        fprintf(stderr, "Error opening file: %s\n", input_filename);
        return;
    }

    TagLib_Tag *tag = taglib_file_tag(file);

    // Define key width for alignment
    const int key_width = 17;

    if (tag) {
        // Print all available standard metadata with alignment
        printf("%-*s: %s\n", key_width, "artist",
               taglib_tag_artist(tag) ? taglib_tag_artist(tag) : "Unknown");
        printf("%-*s: %s\n", key_width, "album",
               taglib_tag_album(tag) ? taglib_tag_album(tag) : "Unknown");
        printf("%-*s: %s\n", key_width, "title",
               taglib_tag_title(tag) ? taglib_tag_title(tag) : "Unknown");
        printf("%-*s: %d\n", key_width, "track", taglib_tag_track(tag));
        printf("%-*s: %d\n", key_width, "year", taglib_tag_year(tag));
        printf("%-*s: %s\n", key_width, "genre",
               taglib_tag_genre(tag) ? taglib_tag_genre(tag) : "Unknown");
        printf("%-*s: %s\n", key_width, "comment",
               taglib_tag_comment(tag) ? taglib_tag_comment(tag) : "None");
    } else {
        printf("No tag information available.\n");
    }

    // Clean up
    taglib_file_free(file);
}

void print_metadata(AVFormatContext *fmt_ctx) {
    AVDictionaryEntry *tag = NULL;
    while ((
        tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        printf("%-17s: %s\n", tag->key, tag->value);
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

    if ((ret = avformat_open_input(&fmt_ctx, input_filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open source file %s\n", input_filename);
        exit(-1);
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(-1);
    }

    if (strcmp(ext, "opus") == 0) {
        taglib_print_metadata(input_filename);
    } else {
        print_metadata(fmt_ctx);
    }

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