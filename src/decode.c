#include "decode.h"
#include "const.h"
#include "util.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

static void decode_print_metadata(AVFormatContext *fmt_ctx) {
    AVDictionaryEntry *tag = NULL;

    // Print format-level metadata
    while ((
        tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        util_tolower(tag->key);
        fprintf(stdout, "  %-*s: %s\n", WIDTH, tag->key, tag->value);
    }
    // Print stream-level metadata
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            util_tolower(tag->key);
            fprintf(stdout, "  %-*s: %s\n", WIDTH, tag->key, tag->value);
        }
    }
}

static int64_t decode_duration(AVFormatContext *fmt_ctx, int stream_index) {
    int64_t duration = -1;
    if (fmt_ctx->streams[stream_index]->duration != AV_NOPTS_VALUE) {
        duration = fmt_ctx->streams[stream_index]->duration *
                   av_q2d(fmt_ctx->streams[stream_index]->time_base);
    } else if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration = fmt_ctx->duration / AV_TIME_BASE;
    }
    return duration;
}

void decode_audio(config_t *config, char *input_filename) {
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
        fprintf(stderr, "Could not open source file %s: %s\n", input_filename,
                av_err2str(ret));
        goto cleanup;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream information: %s\n",
                av_err2str(ret));
        goto cleanup;
    }

    decode_print_metadata(fmt_ctx);

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

    codec_ctx->pkt_timebase = fmt_ctx->streams[stream_index]->time_base;
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

    const int out_channels = 2;
    const int out_samplerate = 48000;
    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, out_channels);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_samplerate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context: %s\n",
                av_err2str(ret));
        goto cleanup;
    }

    // config->pipe_name
    output_fp = fopen(config->pipe_name, "wb");
    if (!output_fp) {
        fprintf(stderr, "Could not open output pipe %s\n", config->pipe_name);
        goto cleanup;
    }

    int64_t duration = decode_duration(fmt_ctx, stream_index);
    char dur_str[9];
    util_seconds_to_time((int)duration, dur_str, sizeof(dur_str));

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_index) {
            ret = avcodec_send_packet(codec_ctx, pkt);
            if (ret < 0) {
                fprintf(stderr,
                        "Error submitting the packet to the decoder: %s\n",
                        av_err2str(ret));
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "Error during decoding: %s\n",
                            av_err2str(ret));
                    goto cleanup;
                }

                uint8_t *output_buffer = NULL;
                int max_dst_nb_samples =
                    av_rescale_rnd(frame->nb_samples, out_samplerate,
                                   codec_ctx->sample_rate, AV_ROUND_UP);
                int output_buffer_size =
                    av_samples_alloc(&output_buffer, NULL, out_channels,
                                     max_dst_nb_samples, AV_SAMPLE_FMT_S16, 0);
                if (output_buffer_size < 0) {
                    fprintf(stderr, "Could not allocate output buffer\n");
                    goto cleanup;
                }

                int nb_samples = swr_convert(
                    swr_ctx, &output_buffer, max_dst_nb_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);
                if (nb_samples < 0) {
                    fprintf(stderr, "Error while converting\n");
                    av_freep(&output_buffer);
                    goto cleanup;
                }

                fwrite(output_buffer, 1,
                       max_dst_nb_samples * out_channels *
                           av_get_bytes_per_sample(AV_SAMPLE_FMT_S16),
                       output_fp);
                av_freep(&output_buffer);

                int64_t current_pts =
                    frame->pts *
                    av_q2d(fmt_ctx->streams[stream_index]->time_base);
                char time_str[9];
                util_seconds_to_time((int)current_pts, time_str,
                                     sizeof(time_str));
                fprintf(stdout, "  %-*s: %s / %s\r", WIDTH, "position",
                        time_str, dur_str);
                fflush(stdout);
            }
        }
        av_packet_unref(pkt);
    }

    fprintf(stdout, "\n\n");

cleanup:
    if (output_fp)
        fclose(output_fp);
    if (frame)
        av_frame_free(&frame);
    if (pkt)
        av_packet_free(&pkt);
    if (codec_ctx)
        avcodec_free_context(&codec_ctx);
    if (fmt_ctx)
        avformat_close_input(&fmt_ctx);
    if (swr_ctx)
        swr_free(&swr_ctx);
}
