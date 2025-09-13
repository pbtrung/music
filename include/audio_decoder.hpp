#pragma once

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace AudioDecoderUtils {

struct AVFormatContextDeleter {
    void operator()(AVFormatContext *ctx) {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext *ctx) {
        if (ctx) {
            avcodec_free_context(&ctx);
        }
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext *ctx) {
        if (ctx) {
            swr_free(&ctx);
        }
    }
};

struct AVPacketDeleter {
    void operator()(AVPacket *pkt) {
        if (pkt) {
            av_packet_free(&pkt);
        }
    }
};

struct AVFrameDeleter {
    void operator()(AVFrame *frame) {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};

class AVSampleBuffer {
  private:
    uint8_t *buffer;

  public:
    AVSampleBuffer() : buffer(nullptr) {}
    ~AVSampleBuffer() {
        if (buffer) {
            av_freep(&buffer);
        }
    }

    AVSampleBuffer(const AVSampleBuffer &) = delete;
    AVSampleBuffer &operator=(const AVSampleBuffer &) = delete;

    AVSampleBuffer(AVSampleBuffer &&other) noexcept : buffer(other.buffer) {
        other.buffer = nullptr;
    }

    AVSampleBuffer &operator=(AVSampleBuffer &&other) noexcept {
        if (this != &other) {
            if (buffer) {
                av_freep(&buffer);
            }
            buffer = other.buffer;
            other.buffer = nullptr;
        }
        return *this;
    }

    bool allocate(int channels, int nb_samples, AVSampleFormat sample_fmt) {
        if (buffer) {
            av_freep(&buffer);
        }
        int ret = av_samples_alloc(&buffer, nullptr, channels, nb_samples,
                                   sample_fmt, 0);
        return ret >= 0;
    }

    uint8_t *get() const {
        return buffer;
    }
    uint8_t **get_ptr() {
        return &buffer;
    }
    bool is_valid() const {
        return buffer != nullptr;
    }
};

using AVFormatContextPtr =
    std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr =
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

} // namespace AudioDecoderUtils

class AudioDecoder {
  public:
    AudioDecoder(std::string pipe_name, std::string filename,
                 std::string file_path);
    ~AudioDecoder() = default;

    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    void decode();

  private:
    AudioDecoderUtils::AVFormatContextPtr fmt_ctx;
    AudioDecoderUtils::AVCodecContextPtr codec_ctx;
    AudioDecoderUtils::SwrContextPtr swr_ctx;
    AudioDecoderUtils::AVPacketPtr pkt;
    AudioDecoderUtils::AVFramePtr frame;

    std::ofstream output_stream;
    std::string pipe_name;
    std::string filename;
    std::string file_path;
    std::string duration_str;

    double current_gain_db = 0.0;
    void apply_gain(uint8_t *buffer, int nb_samples);

    int stream_index = -1;
    static constexpr int width = 1;
    static constexpr int out_channels = 2;
    static constexpr int out_samplerate = 48000;
    static constexpr AVSampleFormat out_samplefmt = AV_SAMPLE_FMT_S16;

    void init();
    void print_metadata();
    int64_t get_duration();
    int find_audio_stream();
    void open_codec();
    void init_resampler();
    void open_output_pipe();
    void print_audio_info();
    void process_frame();

    static void ffmpeg_log_cb(void *avcl, int level, const char *fmt_str,
                              va_list vl);
    void log_duration(std::chrono::steady_clock::time_point start);
};
