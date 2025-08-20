#include <chrono>
#include <fstream>
#include <iostream>

#include <fmt/core.h>
#include <fmt/format.h>

#include <spdlog/spdlog.h>

#include "audio_decoder.hpp"
#include "utils.hpp"

AudioDecoder::AudioDecoder(std::string pipe_name, std::string filename,
                           std::string file_path)
    : pipe_name(std::move(pipe_name)), filename(std::move(filename)),
      file_path(std::move(file_path)) {}

AudioDecoder::~AudioDecoder() = default;

void AudioDecoder::decode() {
    SPDLOG_TRACE("decode_audio: start decoding {}", filename);
    auto start = std::chrono::steady_clock::now();

    try {
        init();
    } catch (const std::runtime_error &e) {
        SPDLOG_TRACE("decode_audio: init failed: {}", e.what());
        return;
    }

    log_duration(start);

    SPDLOG_TRACE("decode_audio: start decode loop");
    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == stream_index) {
            process_frame();
        }
        av_packet_unref(pkt.get());
    }
    fmt::print("\n\n");
    SPDLOG_TRACE("decode_audio: finish decode loop");
    SPDLOG_TRACE("decode_audio: end decoding {}", filename);
}

void AudioDecoder::init() {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log_cb);

    AVFormatContext *tmp_fmt_ctx = nullptr;
    if (avformat_open_input(&tmp_fmt_ctx, file_path.c_str(), nullptr, nullptr) <
        0) {
        throw std::runtime_error("Failed to open source file");
    }
    fmt_ctx.reset(tmp_fmt_ctx);

    if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
        throw std::runtime_error("Failed to find stream information");
    }

    print_metadata();

    stream_index = find_audio_stream();
    if (stream_index == -1) {
        throw std::runtime_error("Failed to find audio stream");
    }

    open_codec();
    init_resampler();
    open_output_pipe();

    pkt.reset(av_packet_alloc());
    if (!pkt)
        throw std::runtime_error("Failed to allocate packet");

    frame.reset(av_frame_alloc());
    if (!frame)
        throw std::runtime_error("Failed to allocate frame");

    int64_t duration = get_duration();
    duration_str = Utilities::format_time(static_cast<int>(duration));

    print_audio_info();
}

void AudioDecoder::print_metadata() {
    AVDictionaryEntry *tag = nullptr;
    while ((
        tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        std::string key_str(tag->key);
        Utilities::to_lower(key_str);
        fmt::print("  {0:<{1}}: {2}\n", key_str, width, tag->value);
    }
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            std::string key_str(tag->key);
            Utilities::to_lower(key_str);
            fmt::print("  {0:<{1}}: {2}\n", key_str, width, tag->value);
        }
    }
}

int64_t AudioDecoder::get_duration() {
    if (fmt_ctx->streams[stream_index]->duration != AV_NOPTS_VALUE) {
        return fmt_ctx->streams[stream_index]->duration *
               av_q2d(fmt_ctx->streams[stream_index]->time_base);
    } else if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        return fmt_ctx->duration / AV_TIME_BASE;
    }
    return -1;
}

int AudioDecoder::find_audio_stream() {
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            return i;
        }
    }
    return -1;
}

void AudioDecoder::open_codec() {
    const AVCodec *codec = avcodec_find_decoder(
        fmt_ctx->streams[stream_index]->codecpar->codec_id);
    if (!codec)
        throw std::runtime_error("Failed to find codec");

    AVCodecContext *tmp_codec_ctx = avcodec_alloc_context3(codec);
    if (!tmp_codec_ctx)
        throw std::runtime_error("Failed to allocate codec context");

    codec_ctx.reset(tmp_codec_ctx);

    if (avcodec_parameters_to_context(
            codec_ctx.get(), fmt_ctx->streams[stream_index]->codecpar) < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    codec_ctx->pkt_timebase = fmt_ctx->streams[stream_index]->time_base;
    if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
        throw std::runtime_error("Failed to open codec");
    }
}

void AudioDecoder::init_resampler() {
    SwrContext *tmp_swr = swr_alloc();
    if (!tmp_swr)
        throw std::runtime_error("Failed to allocate resampler");

    swr_ctx.reset(tmp_swr);

    av_opt_set_chlayout(swr_ctx.get(), "in_chlayout", &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx.get(), "in_sample_rate", codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx.get(), "in_sample_fmt", codec_ctx->sample_fmt,
                          0);

    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, out_channels);
    av_opt_set_chlayout(swr_ctx.get(), "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_ctx.get(), "out_sample_rate", out_samplerate, 0);
    av_opt_set_sample_fmt(swr_ctx.get(), "out_sample_fmt", out_samplefmt, 0);
    av_opt_set(swr_ctx.get(), "resampler", "soxr", 0);
    av_opt_set_int(swr_ctx.get(), "precision", 20, 0);

    if (swr_init(swr_ctx.get()) < 0) {
        throw std::runtime_error("Failed to initialize resampler");
    }
}

void AudioDecoder::open_output_pipe() {
    output_stream.open(pipe_name, std::ios::binary);
    if (!output_stream.is_open()) {
        throw std::runtime_error("Failed to open output pipe");
    }
}

void AudioDecoder::print_audio_info() {
    fmt::print("  {0:<{1}}: {2}\n", "codec", width,
               codec_ctx->codec->long_name);
    if (codec_ctx->bit_rate != 0) {
        fmt::print("  {0:<{1}}: {2} kbps\n", "bit-rate", width,
                   codec_ctx->bit_rate / 1000);
    }
    fmt::print("  {0:<{1}}: {2}\n", "sample-rate", width,
               codec_ctx->sample_rate);

    char sample_fmt_str[16];
    av_get_sample_fmt_string(sample_fmt_str, sizeof(sample_fmt_str),
                             codec_ctx->sample_fmt);
    std::string fmt_str(sample_fmt_str);
    Utilities::trim_spaces(fmt_str);
    fmt::print("  {0:<{1}}: {2}\n", "sample-fmt", width, fmt_str);
    fmt::print("  {0:<{1}}: {2}\n", "channels", width,
               codec_ctx->ch_layout.nb_channels);

    if (codec_ctx->ch_layout.nb_channels != out_channels) {
        fmt::print("  {:<{}}: {} -> {}\n", "resample-channels", width,
                   codec_ctx->ch_layout.nb_channels, out_channels);
    }
    if (codec_ctx->sample_rate != out_samplerate) {
        fmt::print("  {:<{}}: {} -> {}\n", "resample-rate", width,
                   codec_ctx->sample_rate, out_samplerate);
    }
}

void AudioDecoder::process_frame() {
    int ret = avcodec_send_packet(codec_ctx.get(), pkt.get());
    if (ret < 0)
        throw std::runtime_error("Failed to send packet");

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx.get(), frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
            throw std::runtime_error("Error during decoding");

        uint8_t *output_buffer = nullptr;
        int max_dst_nb_samples =
            av_rescale_rnd(frame->nb_samples, out_samplerate,
                           codec_ctx->sample_rate, AV_ROUND_UP);
        int output_buffer_size =
            av_samples_alloc(&output_buffer, nullptr, out_channels,
                             max_dst_nb_samples, out_samplefmt, 0);
        if (output_buffer_size < 0)
            throw std::runtime_error("Failed to allocate output buffer");

        int nb_samples =
            swr_convert(swr_ctx.get(), &output_buffer, max_dst_nb_samples,
                        (const uint8_t **)frame->data, frame->nb_samples);
        if (nb_samples < 0) {
            av_freep(&output_buffer);
            throw std::runtime_error("Error converting samples");
        }

        output_stream.write(reinterpret_cast<char *>(output_buffer),
                            max_dst_nb_samples * out_channels *
                                av_get_bytes_per_sample(out_samplefmt));
        av_freep(&output_buffer);

        int64_t current_pts =
            frame->pts * av_q2d(fmt_ctx->streams[stream_index]->time_base);
        std::string current_time_str =
            Utilities::format_time(static_cast<int>(current_pts));
        fmt::print("  {0:<{1}}: {2} / {3}\r", "position", width,
                   current_time_str, duration_str);
        std::cout.flush();
    }
}

void AudioDecoder::ffmpeg_log_cb(void *avcl, int level, const char *fmt_str,
                                 va_list vl) {
    if (level <= av_log_get_level()) {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt_str, vl);
        SPDLOG_TRACE("{}", buffer);
    }
}

void AudioDecoder::log_duration(std::chrono::steady_clock::time_point start) {
    auto end = std::chrono::steady_clock::now();
    double elapsed_time =
        std::chrono::duration<double, std::milli>(end - start).count();
    fmt::print("  {0:<{1}}: {2:.3f} ms\n", "took", width, elapsed_time);
}
