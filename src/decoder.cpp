#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <fmt/core.h>
#include <fmt/format.h>
#include <iostream>
#include <stdexcept>

decoder::decoder(const std::string &input_filename,
                 const std::string &output_pipe)
    : input_filename_(input_filename), output_pipe_(output_pipe) {
    init();
}

decoder::~decoder() { cleanup(); }

void decoder::init() {
    av_log_set_level(AV_LOG_ERROR);

    if (avformat_open_input(&fmt_ctx_, input_filename_.c_str(), nullptr,
                            nullptr) < 0) {
        throw std::runtime_error(
            fmt::format("Could not open source file {}", input_filename_));
    }

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        throw std::runtime_error("Could not find stream information");
    }

    for (int i = 0; i < fmt_ctx_->nb_streams; i++) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stream_index_ = i;
            break;
        }
    }

    if (stream_index_ == -1) {
        throw std::runtime_error("Could not find audio stream");
    }

    const AVCodec *codec = avcodec_find_decoder(
        fmt_ctx_->streams[stream_index_]->codecpar->codec_id);
    if (!codec) {
        throw std::runtime_error("Failed to find codec");
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        throw std::runtime_error("Failed to allocate codec");
    }

    if (avcodec_parameters_to_context(
            codec_ctx_, fmt_ctx_->streams[stream_index_]->codecpar) < 0) {
        throw std::runtime_error(
            "Failed to copy codec parameters to decoder context");
    }

    codec_ctx_->pkt_timebase = fmt_ctx_->streams[stream_index_]->time_base;
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        throw std::runtime_error("Failed to open codec");
    }

    packet_ = av_packet_alloc();
    if (!packet_) {
        throw std::runtime_error("Could not allocate AVPacket");
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("Could not allocate AVFrame");
    }

    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        throw std::runtime_error("Could not allocate SwrContext");
    }

    av_opt_set_chlayout(swr_ctx_, "in_chlayout", &codec_ctx_->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", codec_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", codec_ctx_->sample_fmt, 0);

    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, 2);
    av_opt_set_chlayout(swr_ctx_, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", 48000, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(swr_ctx_) < 0) {
        throw std::runtime_error("Failed to initialize the resampling context");
    }

    output_fp_.open(output_pipe_, std::ios::binary);
    if (!output_fp_) {
        throw std::runtime_error(
            fmt::format("Could not open output pipe {}", output_pipe_));
    }

    // Accurate duration calculation for VBR
    if (fmt_ctx_->streams[stream_index_]->duration != AV_NOPTS_VALUE) {
        duration_ = fmt_ctx_->streams[stream_index_]->duration *
                    av_q2d(fmt_ctx_->streams[stream_index_]->time_base);
    } else if (fmt_ctx_->duration != AV_NOPTS_VALUE) {
        duration_ = fmt_ctx_->duration / AV_TIME_BASE;
    } else {
        duration_ = -1; // Unknown duration
    }

    dur_str = utils::get_time(static_cast<double>(duration_));
}

void decoder::cleanup() {
    if (output_fp_.is_open()) {
        output_fp_.close();
    }
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_ctx_);
    avformat_close_input(&fmt_ctx_);
    swr_free(&swr_ctx_);
}

void decoder::print_metadata() const {
    AVDictionaryEntry *tag = nullptr;

    // Print format-level metadata
    while ((tag = av_dict_get(fmt_ctx_->metadata, "", tag,
                              AV_DICT_IGNORE_SUFFIX))) {
        std::string key = tag->key ? tag->key : "";
        std::string value = tag->value ? tag->value : "";
        if (!key.empty() && !value.empty()) {
            utils::to_lowercase(key);
            fmt::print("  {:<{}}: {}\n", key, WIDTH, value);
        }
    }

    // Print stream-level metadata
    for (int i = 0; i < fmt_ctx_->nb_streams; i++) {
        AVStream *stream = fmt_ctx_->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            std::string key = tag->key ? tag->key : "";
            std::string value = tag->value ? tag->value : "";
            if (!key.empty() && !value.empty()) {
                utils::to_lowercase(key);
                fmt::print("  {:<{}}: {}\n", key, WIDTH, value);
            }
        }
    }
}

void decoder::decode() {
    // Read and decode frames
    while (av_read_frame(fmt_ctx_, packet_) >= 0) {
        if (packet_->stream_index == stream_index_) {
            // Key Change: Check for packet duration and adjust if needed
            if (packet_->duration > 0) {
                packet_->duration =
                    av_rescale_q(packet_->duration,
                                 fmt_ctx_->streams[stream_index_]->time_base,
                                 codec_ctx_->time_base);
            }

            decode_frame();
        }
        av_packet_unref(packet_);
    }

    // Flush the decoder
    flush_decoder();

    fmt::print(stdout, "\n\n");
}

void decoder::decode_frame() {
    if (avcodec_send_packet(codec_ctx_, packet_) < 0) {
        throw std::runtime_error("Error sending packet to decoder");
    }

    while (true) {
        int ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            throw std::runtime_error("Error during decoding");
        }

        process_frame();
    }
}

void decoder::process_frame() {
    uint8_t *output_buffer = nullptr;
    int output_buffer_size = av_samples_alloc(
        &output_buffer, nullptr, 2, frame_->nb_samples, AV_SAMPLE_FMT_S16, 0);

    if (output_buffer_size < 0) {
        throw std::runtime_error("Could not allocate output buffer");
    }

    int nb_samples =
        swr_convert(swr_ctx_, &output_buffer, frame_->nb_samples,
                    (const uint8_t **)frame_->data, frame_->nb_samples);

    if (nb_samples < 0) {
        av_freep(&output_buffer);
        throw std::runtime_error("Error while converting");
    }

    output_fp_.write(reinterpret_cast<char *>(output_buffer),
                     nb_samples * 2 *
                         av_get_bytes_per_sample(AV_SAMPLE_FMT_S16));
    av_freep(&output_buffer);

    int64_t current_pts =
        frame_->pts * av_q2d(fmt_ctx_->streams[stream_index_]->time_base);
    std::string cur_str = utils::get_time(static_cast<double>(current_pts));
    fmt::print(stdout, "  {:<{}}: {} / {}\r", "position", WIDTH, cur_str,
               dur_str);
    std::cout.flush();
}

void decoder::flush_decoder() {
    if (avcodec_send_packet(codec_ctx_, nullptr) >= 0) {
        while (true) {
            int ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                throw std::runtime_error("Error during flushing");
            }

            process_frame();
        }
    }
}