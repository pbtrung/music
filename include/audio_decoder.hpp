#pragma once

#include <chrono>
#include <cstdarg>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

class AudioDecoder {
  public:
    AudioDecoder(std::string pipe_name, std::string filename,
                 std::string file_path);
    ~AudioDecoder();

    // Delete copy operations
    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    // Allow move operations
    AudioDecoder(AudioDecoder &&) = default;
    AudioDecoder &operator=(AudioDecoder &&) = default;

    void decode();

  private:
    // Configuration constants
    static constexpr int width = 1;
    static constexpr int out_channels = 2;
    static constexpr int out_samplerate = 48000;
    static constexpr AVSampleFormat out_samplefmt = AV_SAMPLE_FMT_S16;

    // Member variables
    std::string pipe_name;
    std::string filename;
    std::string file_path;
    std::string duration_str;

    // FFmpeg resources with custom deleters
    struct AVFormatContextDeleter {
        void operator()(AVFormatContext *ctx) const {
            if (ctx)
                avformat_close_input(&ctx);
        }
    };

    struct AVCodecContextDeleter {
        void operator()(AVCodecContext *ctx) const {
            if (ctx)
                avcodec_free_context(&ctx);
        }
    };

    struct AVPacketDeleter {
        void operator()(AVPacket *pkt) const {
            if (pkt)
                av_packet_free(&pkt);
        }
    };

    struct AVFrameDeleter {
        void operator()(AVFrame *frame) const {
            if (frame)
                av_frame_free(&frame);
        }
    };

    struct SwrContextDeleter {
        void operator()(SwrContext *ctx) const {
            if (ctx)
                swr_free(&ctx);
        }
    };

    // Smart pointers for automatic resource management
    std::unique_ptr<AVFormatContext, AVFormatContextDeleter> fmt_ctx;
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_ctx;
    std::unique_ptr<AVPacket, AVPacketDeleter> pkt;
    std::unique_ptr<AVFrame, AVFrameDeleter> frame;
    std::unique_ptr<SwrContext, SwrContextDeleter> swr_ctx;

    std::ofstream output_stream;

    int stream_index = -1;

    // Private methods
    void init();
    void print_metadata();
    int64_t get_duration();
    int find_audio_stream();
    void open_codec();
    void init_resampler();
    void open_output_pipe();
    void print_audio_info();
    void process_frame();
    void log_duration(std::chrono::steady_clock::time_point start);

    // Static callback function
    static void ffmpeg_log_cb(void *avcl, int level, const char *fmt,
                              va_list vl);
};
