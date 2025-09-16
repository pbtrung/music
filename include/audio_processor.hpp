#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>

extern "C" {
#include <ebur128.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace fs = std::filesystem;

namespace AudioUtils {

// RAII wrappers for FFmpeg resources
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

using AVFormatContextPtr =
    std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr =
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// Sample buffer management with RAII
class SampleBuffer {
  public:
    SampleBuffer() : buffer(nullptr) {}
    ~SampleBuffer() {
        if (buffer) {
            av_freep(&buffer);
        }
    }

    SampleBuffer(const SampleBuffer &) = delete;
    SampleBuffer &operator=(const SampleBuffer &) = delete;

    SampleBuffer(SampleBuffer &&other) noexcept : buffer(other.buffer) {
        other.buffer = nullptr;
    }

    SampleBuffer &operator=(SampleBuffer &&other) noexcept {
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

  private:
    uint8_t *buffer;
};

// Audio processing configuration
struct AudioConfig {
    int channels = 2;
    int sample_rate = 48000;
    AVSampleFormat sample_format = AV_SAMPLE_FMT_S16;
    std::string resampler = "soxr";
    int precision = 20;
};

// Callback for processing audio samples
using AudioSampleCallback =
    std::function<void(uint8_t *buffer, int nb_samples, int channels)>;

} // namespace AudioUtils

// Base class for audio processing
class AudioProcessorBase {
  public:
    explicit AudioProcessorBase(const fs::path &file_path);
    virtual ~AudioProcessorBase() = default;

    AudioProcessorBase(const AudioProcessorBase &) = delete;
    AudioProcessorBase &operator=(const AudioProcessorBase &) = delete;
    AudioProcessorBase(AudioProcessorBase &&) = default;
    AudioProcessorBase &operator=(AudioProcessorBase &&) = default;

  protected:
    // Core initialization methods
    void initialize_ffmpeg();
    void find_audio_stream();
    void initialize_codec();
    void initialize_resampler(const AudioUtils::AudioConfig &config);

    // Audio processing
    void process_audio_data(const AudioUtils::AudioSampleCallback &callback);

    // Utility methods
    void print_metadata() const;
    void print_audio_info(const AudioUtils::AudioConfig &config) const;
    int64_t get_duration() const;

    // Static utility
    static void setup_ffmpeg_logging();

    // Member variables
    fs::path file_path;
    AudioUtils::AVFormatContextPtr format_context;
    AudioUtils::AVCodecContextPtr codec_context;
    AudioUtils::SwrContextPtr swr_context;
    AudioUtils::AVPacketPtr packet;
    AudioUtils::AVFramePtr frame;
    int audio_stream_index = -1;

  private:
    static void ffmpeg_log_callback(void *avcl, int level, const char *fmt,
                                    va_list vl);
};

// SIMD-optimized gain processor
class GainProcessor {
  public:
    explicit GainProcessor(int gain_fixed);

    void apply_gain(uint8_t *buffer, int nb_samples, int channels);

  private:
    int gain_fixed;

    // CPU feature detection
    static bool features_checked;
    static bool has_avx512;
    static bool has_avx2;
    static bool has_sse2;
    static bool has_neon;
    static void (*apply_gain_impl)(int16_t *samples, int total_samples,
                                   int gain_fixed);

    static void detect_cpu_features();

    // SIMD implementations
#if defined(__x86_64__) || defined(_M_X64)
    static void apply_gain_avx512_impl(int16_t *samples, int total_samples,
                                       int gain_fixed);
    static void apply_gain_avx2_impl(int16_t *samples, int total_samples,
                                     int gain_fixed);
    static void apply_gain_sse2_impl(int16_t *samples, int total_samples,
                                     int gain_fixed);
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    static void apply_gain_neon_impl(int16_t *samples, int total_samples,
                                     int gain_fixed);
#endif

    static void apply_gain_scalar_impl(int16_t *samples, int total_samples,
                                       int gain_fixed);
};

// Audio decoder
class AudioDecoder : public AudioProcessorBase {
  public:
    AudioDecoder(const std::string &pipe_name, const fs::path &file_path,
                 int gain_fixed);

    void decode();

  private:
    std::ofstream output_stream;
    std::string pipe_name;
    std::string duration_str;
    GainProcessor gain_processor;

    static constexpr AudioUtils::AudioConfig OUTPUT_CONFIG{
        .channels = 2,
        .sample_rate = 48000,
        .sample_format = AV_SAMPLE_FMT_S16,
        .resampler = "soxr",
        .precision = 20};

    void initialize();
    void open_output_pipe();
    void log_duration(std::chrono::steady_clock::time_point start) const;
};

// Track gain analyzer
class TrackGainAnalyzer : public AudioProcessorBase {
  public:
    static double compute_and_write_track_gain(const fs::path &file_path);

    explicit TrackGainAnalyzer(const fs::path &file_path);

    double compute_track_gain();
    void write_track_gain_tag(double gain_value);

  private:
    struct EbuStateDeleter {
        void operator()(ebur128_state *state) {
            if (state) {
                ebur128_destroy(&state);
            }
        }
    };

    using EbuStatePtr = std::unique_ptr<ebur128_state, EbuStateDeleter>;

    EbuStatePtr ebu_state;

    static constexpr AudioUtils::AudioConfig ANALYSIS_CONFIG{
        .channels = 2,
        .sample_rate = 48000,
        .sample_format = AV_SAMPLE_FMT_DBL,
        .resampler = "soxr",
        .precision = 20};

    static constexpr double REFERENCE_LOUDNESS = -23.0; // LUFS

    void initialize_ebur128();
    void write_metadata_tag(const std::string &key, const std::string &value);
};