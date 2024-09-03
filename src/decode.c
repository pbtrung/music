#include "decode.h"
#include "const.h"
#include "log.h"
#include "util.h"
#include <apr_time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#define OUT_SAMPLERATE 48000
#define OUT_SAMPLEFMT AV_SAMPLE_FMT_S16
#define OUT_CHANNELS 2

static void decode_print_metadata(AVFormatContext *fmt_ctx) {
    AVDictionaryEntry *tag = NULL;

    while ((
        tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        util_tolower(tag->key);
        log_trace("%s: %s", tag->key, tag->value);
        fprintf(stdout, "  %-*s: %s\n", WIDTH, tag->key, tag->value);
    }
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            util_tolower(tag->key);
            log_trace("%s: %s", tag->key, tag->value);
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

static int decode_find_audio_stream(AVFormatContext *fmt_ctx) {
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            return i;
        }
    }
    return -1;
}

static AVCodecContext *decode_open_codec(AVFormatContext *fmt_ctx,
                                         int stream_index) {
    const AVCodec *codec = avcodec_find_decoder(
        fmt_ctx->streams[stream_index]->codecpar->codec_id);
    if (!codec) {
        log_trace("decode_open_codec: Failed to find codec");
        return NULL;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Failed to allocate codec context\n");
        return NULL;
    }

    if (avcodec_parameters_to_context(
            codec_ctx, fmt_ctx->streams[stream_index]->codecpar) < 0) {
        log_trace(
            "decode_open_codec: Failed to copy codec parameters to decoder context");
        avcodec_free_context(&codec_ctx);
        return NULL;
    }

    codec_ctx->pkt_timebase = fmt_ctx->streams[stream_index]->time_base;
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        log_trace("decode_open_codec: Failed to open codec");
        avcodec_free_context(&codec_ctx);
        return NULL;
    }

    return codec_ctx;
}

static SwrContext *decode_initialize_resampler(AVCodecContext *codec_ctx) {
    SwrContext *swr_ctx = swr_alloc();
    if (!swr_ctx) {
        log_trace(
            "decode_initialize_resampler: Failed to allocate the resampling context");
        return NULL;
    }

    av_opt_set_chlayout(swr_ctx, "in_chlayout", &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);

    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, OUT_CHANNELS);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", OUT_SAMPLERATE, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", OUT_SAMPLEFMT, 0);
    av_opt_set(swr_ctx, "resampler", "soxr", 0);
    av_opt_set_int(swr_ctx, "precision", 20, 0);

    if (swr_init(swr_ctx) < 0) {
        log_trace(
            "decode_initialize_resampler: Failed to initialize the resampling context");
        swr_free(&swr_ctx);
        return NULL;
    }

    return swr_ctx;
}

static FILE *decode_open_output_pipe(char *pipe_name) {
    FILE *output_fp = fopen(pipe_name, "wb");
    if (!output_fp) {
        log_trace("decode_open_output_pipe: Failed to open output pipe: %s",
                  pipe_name);
    }
    return output_fp;
}

static void decode_print_audio_info(AVCodecContext *codec_ctx) {
    log_trace("codec: %s", codec_ctx->codec->long_name);
    fprintf(stdout, "  %-*s: %s\n", WIDTH, "codec",
            codec_ctx->codec->long_name);
    if (codec_ctx->bit_rate != 0) {
        log_trace("bit-rate: %ld", (long)codec_ctx->bit_rate / 1000);
        fprintf(stdout, "  %-*s: %ld kbps\n", WIDTH, "bit-rate",
                (long)codec_ctx->bit_rate / 1000);
    }
    log_trace("sample-rate: %d", codec_ctx->sample_rate);
    fprintf(stdout, "  %-*s: %d\n", WIDTH, "sample-rate",
            codec_ctx->sample_rate);

    char sample_fmt[16];
    av_get_sample_fmt_string(sample_fmt, sizeof(sample_fmt),
                             codec_ctx->sample_fmt);
    util_remove_spaces(sample_fmt);
    log_trace("sample-fmt: %s", sample_fmt);
    fprintf(stdout, "  %-*s: %s\n", WIDTH, "sample-fmt", sample_fmt);
    log_trace("channels: %d", codec_ctx->ch_layout.nb_channels);
    fprintf(stdout, "  %-*s: %d\n", WIDTH, "channels",
            codec_ctx->ch_layout.nb_channels);

    if (codec_ctx->ch_layout.nb_channels != OUT_CHANNELS) {
        log_trace("resample: %d -> %d", codec_ctx->ch_layout.nb_channels,
                  OUT_CHANNELS);
        fprintf(stdout, "  %-*s: %d -> %d\n", WIDTH, "resample",
                codec_ctx->ch_layout.nb_channels, OUT_CHANNELS);
    }
    if (codec_ctx->sample_rate != OUT_SAMPLERATE) {
        log_trace("resample: %d -> %d", codec_ctx->sample_rate, OUT_SAMPLERATE);
        fprintf(stdout, "  %-*s: %d -> %d\n", WIDTH, "resample",
                codec_ctx->sample_rate, OUT_SAMPLERATE);
    }
}

static int decode_process_frame(AVFormatContext *fmt_ctx,
                                AVCodecContext *codec_ctx, SwrContext *swr_ctx,
                                AVFrame *frame, AVPacket *pkt, FILE *output_fp,
                                const char *dur_str, int stream_index) {
    int ret;
    ret = avcodec_send_packet(codec_ctx, pkt);
    if (ret < 0) {
        log_trace(
            "decode_process_frame: Failed to submit the packet to the decoder: %s",
            av_err2str(ret));
        return ret;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            log_trace("decode_process_frame: Error during decoding: %s",
                      av_err2str(ret));
            return ret;
        }

        uint8_t *output_buffer = NULL;
        int max_dst_nb_samples =
            av_rescale_rnd(frame->nb_samples, OUT_SAMPLERATE,
                           codec_ctx->sample_rate, AV_ROUND_UP);
        int output_buffer_size =
            av_samples_alloc(&output_buffer, NULL, OUT_CHANNELS,
                             max_dst_nb_samples, OUT_SAMPLEFMT, 0);
        if (output_buffer_size < 0) {
            log_trace("decode_process_frame: Failed to allocate output buffer");
            return -1;
        }

        int nb_samples =
            swr_convert(swr_ctx, &output_buffer, max_dst_nb_samples,
                        (const uint8_t **)frame->data, frame->nb_samples);
        if (nb_samples < 0) {
            log_trace("decode_process_frame: Error while converting");
            av_freep(&output_buffer);
            return -1;
        }

        fwrite(output_buffer, 1,
               max_dst_nb_samples * OUT_CHANNELS *
                   av_get_bytes_per_sample(OUT_SAMPLEFMT),
               output_fp);
        av_freep(&output_buffer);

        int64_t current_pts =
            frame->pts * av_q2d(fmt_ctx->streams[stream_index]->time_base);
        char time_str[9];
        util_seconds_to_time((int)current_pts, time_str, sizeof(time_str));
        fprintf(stdout, "  %-*s: %s / %s\r", WIDTH, "position", time_str,
                dur_str);
        fflush(stdout);
    }

    return 0;
}

static void ffmpeg_log_cb(void *avcl, int level, const char *fmt, va_list vl) {
    if (level <= av_log_get_level()) {
        log_trace(fmt, vl);
    }
}

static void log_duration(apr_time_t start) {
    apr_time_t end = apr_time_now();
    apr_time_t diff_usec = end - start;
    double elapsed_time = (double)diff_usec / 1000;
    log_trace("took: %.3f ms", elapsed_time);
    fprintf(stdout, "  %-*s: %.3f ms\n", WIDTH, "took", elapsed_time);
}

static int decode_init(AVFormatContext **fmt_ctx, AVCodecContext **codec_ctx,
                       AVPacket **pkt, AVFrame **frame, SwrContext **swr_ctx,
                       FILE **output_fp, int *stream_index, char *pipe_name,
                       char *file_path, char *dur_str) {
    int ret;

    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log_cb);

    if ((ret = avformat_open_input(fmt_ctx, file_path, NULL, NULL)) < 0) {
        log_trace("Failed to open source file '%s': %s", file_path,
                  av_err2str(ret));
        return ret;
    }

    if ((ret = avformat_find_stream_info(*fmt_ctx, NULL)) < 0) {
        log_trace("Failed to find stream information: %s", av_err2str(ret));
        return ret;
    }

    decode_print_metadata(*fmt_ctx);

    *stream_index = decode_find_audio_stream(*fmt_ctx);
    if (*stream_index == -1) {
        log_trace("Failed to find audio stream");
        return -1;
    }

    *codec_ctx = decode_open_codec(*fmt_ctx, *stream_index);
    if (!*codec_ctx) {
        return -1;
    }

    *swr_ctx = decode_initialize_resampler(*codec_ctx);
    if (!*swr_ctx) {
        return -1;
    }

    *output_fp = decode_open_output_pipe(pipe_name);
    if (!*output_fp) {
        return -1;
    }

    *pkt = av_packet_alloc();
    if (!*pkt) {
        log_trace("Failed to allocate packet");
        return -1;
    }

    *frame = av_frame_alloc();
    if (!*frame) {
        log_trace("Failed to allocate frame");
        return -1;
    }

    int64_t duration = decode_duration(*fmt_ctx, *stream_index);
    util_seconds_to_time((int)duration, dur_str, sizeof(dur_str));
    decode_print_audio_info(*codec_ctx);

    return 0;
}

void decode_audio(char *pipe_name, char *filename, char *file_path) {
    log_trace("decode_audio: start decoding %s", filename);
    apr_time_t start = apr_time_now();

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr_ctx = NULL;
    FILE *output_fp = NULL;
    int stream_index = -1;
    int ret;
    char dur_str[10];

    if ((ret = decode_init(&fmt_ctx, &codec_ctx, &pkt, &frame, &swr_ctx,
                           &output_fp, &stream_index, pipe_name, file_path,
                           dur_str)) < 0) {
        goto cleanup;
    }

    log_duration(start);

    log_trace("decode_audio: start decode loop");
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_index) {
            ret = decode_process_frame(fmt_ctx, codec_ctx, swr_ctx, frame, pkt,
                                       output_fp, dur_str, stream_index);
            if (ret < 0) {
                break;
            }
        }
        av_packet_unref(pkt);
    }
    log_trace("decode_audio: finish decode loop");

    fprintf(stdout, "\n\n");

cleanup:
    if (output_fp)
        fclose(output_fp);
    if (frame)
        av_frame_free(&frame);
    if (pkt) {
        // av_packet_free calls av_packet_unref
        av_packet_free(&pkt);
    }
    if (codec_ctx)
        avcodec_free_context(&codec_ctx);
    if (fmt_ctx)
        avformat_close_input(&fmt_ctx);
    if (swr_ctx)
        swr_free(&swr_ctx);
    log_trace("decode_audio: finish decoding %s", filename);
}
