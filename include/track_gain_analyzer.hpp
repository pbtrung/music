#pragma once

#include <filesystem>
#include <memory>

extern "C" {
#include <ebur128.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include <taglib/fileref.h>
#include <taglib/tag.h>

class TrackGainAnalyzer {
  public:
    // Static method to compute and write track gain
    static double
    compute_and_write_track_gain(const std::filesystem::path &file_path);

    // Constructor for instance-based usage
    explicit TrackGainAnalyzer(const std::filesystem::path &file_path);
    ~TrackGainAnalyzer() = default;

    TrackGainAnalyzer(const TrackGainAnalyzer &) = delete;
    TrackGainAnalyzer &operator=(const TrackGainAnalyzer &) = delete;
    TrackGainAnalyzer(TrackGainAnalyzer &&) = default;
    TrackGainAnalyzer &operator=(TrackGainAnalyzer &&) = default;

    // Compute track gain value
    double compute_track_gain();

    // Write R128_TRACK_GAIN tag to file
    void write_track_gain_tag(double gain_value);

  private:
    // RAII wrappers for FFmpeg resources
    struct FormatContextDeleter {
        void operator()(AVFormatContext *ctx) {
            if (ctx) {
                avformat_close_input(&ctx);
            }
        }
    };

    struct CodecContextDeleter {
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

    struct PacketDeleter {
        void operator()(AVPacket *pkt) {
            if (pkt) {
                av_packet_free(&pkt);
            }
        }
    };

    struct FrameDeleter {
        void operator()(AVFrame *frame) {
            if (frame) {
                av_frame_free(&frame);
            }
        }
    };

    struct EbuStateDeleter {
        void operator()(ebur128_state *state) {
            if (state) {
                ebur128_destroy(&state);
            }
        }
    };

    using FormatContextPtr =
        std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using CodecContextPtr =
        std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using EbuStatePtr = std::unique_ptr<ebur128_state, EbuStateDeleter>;

    // Sample buffer management
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

    // Member variables
    std::filesystem::path file_path;
    FormatContextPtr format_context;
    CodecContextPtr codec_context;
    SwrContextPtr swr_context;
    PacketPtr packet;
    FramePtr frame;
    EbuStatePtr ebu_state;
    int audio_stream_index;

    // Analysis parameters
    static constexpr int target_sample_rate = 48000;
    static constexpr int target_channels = 2;
    static constexpr AVSampleFormat target_sample_format = AV_SAMPLE_FMT_DBL;
    static constexpr double reference_loudness = -23.0; // LUFS

    // Private methods
    void initialize_ffmpeg();
    void find_audio_stream();
    void initialize_codec();
    void initialize_resampler();
    void initialize_ebur128();
    void process_audio_data();
    void write_metadata_tag(const std::string &key, const std::string &value);
    static void setup_ffmpeg_logging();
};