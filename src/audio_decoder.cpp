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

void AudioDecoder::decode() {
    SPDLOG_TRACE("Start decoding {}", filename);
    auto start = std::chrono::steady_clock::now();

    try {
        init();
    } catch (const std::runtime_error &e) {
        SPDLOG_TRACE("init failed: {}", e.what());
        return;
    }

    log_duration(start);

    SPDLOG_TRACE("Start decode loop");
    while (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == stream_index) {
            process_frame();
        }
        av_packet_unref(pkt.get());
    }
    fmt::print("\n\n");
    SPDLOG_TRACE("Finish decode loop");
    SPDLOG_TRACE("End decoding {}", filename);
}

void AudioDecoder::init() {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log_cb);

    AVFormatContext *tmp_fmt_ctx = nullptr;
    if (avformat_open_input(&tmp_fmt_ctx, file_path.c_str(), nullptr, nullptr) <
        0) {
        throw std::runtime_error("Failed to open source file");
    }
    fmt_ctx = AudioDecoderUtils::AVFormatContextPtr(tmp_fmt_ctx);

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

    AVPacket *tmp_pkt = av_packet_alloc();
    if (!tmp_pkt)
        throw std::runtime_error("Failed to allocate packet");
    pkt = AudioDecoderUtils::AVPacketPtr(tmp_pkt);

    AVFrame *tmp_frame = av_frame_alloc();
    if (!tmp_frame)
        throw std::runtime_error("Failed to allocate frame");
    frame = AudioDecoderUtils::AVFramePtr(tmp_frame);

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
        fmt::print("  {:<{}}: {}\n", key_str, width, tag->value);
    }
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            std::string key_str(tag->key);
            Utilities::to_lower(key_str);
            fmt::print("  {:<{}}: {}\n", key_str, width, tag->value);
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
    codec_ctx = AudioDecoderUtils::AVCodecContextPtr(tmp_codec_ctx);

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
    swr_ctx = AudioDecoderUtils::SwrContextPtr(tmp_swr);

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
    fmt::print("  {:<{}}: {}\n", "codec", width, codec_ctx->codec->long_name);
    if (codec_ctx->bit_rate != 0) {
        fmt::print("  {:<{}}: {} kbps\n", "bit-rate", width,
                   codec_ctx->bit_rate / 1000);
    }
    fmt::print("  {:<{}}: {}\n", "sample-rate", width, codec_ctx->sample_rate);

    char sample_fmt_str[16];
    av_get_sample_fmt_string(sample_fmt_str, sizeof(sample_fmt_str),
                             codec_ctx->sample_fmt);
    std::string fmt_str(sample_fmt_str);
    Utilities::trim_spaces(fmt_str);
    fmt::print("  {:<{}}: {}\n", "sample-fmt", width, fmt_str);
    fmt::print("  {:<{}}: {}\n", "channels", width,
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

// ==================================================================

// Static member definitions
bool AudioDecoder::features_checked = false;
bool AudioDecoder::has_avx512 = false;
bool AudioDecoder::has_avx2 = false;
bool AudioDecoder::has_sse2 = false;
bool AudioDecoder::has_neon = false;
void (*AudioDecoder::apply_gain_impl)(int16_t *, int, int) = nullptr;

void AudioDecoder::detect_cpu_features() {
    if (features_checked)
        return;

    const char *selected_impl = "unknown";

#if defined(__x86_64__) || defined(_M_X64)
    // x86_64 detection
    has_sse2 = __builtin_cpu_supports("sse2");
    has_avx2 = __builtin_cpu_supports("avx2");
    has_avx512 = __builtin_cpu_supports("avx512f");
    has_neon = false;

    // Select best implementation
    if (has_avx512) {
        apply_gain_impl = apply_gain_avx512_impl;
        selected_impl = "AVX-512";
    } else if (has_avx2) {
        apply_gain_impl = apply_gain_avx2_impl;
        selected_impl = "AVX2";
    } else if (has_sse2) {
        apply_gain_impl = apply_gain_sse2_impl;
        selected_impl = "SSE2";
    } else {
        apply_gain_impl = apply_gain_scalar_impl;
        selected_impl = "Scalar";
    }

#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM64 detection
    has_sse2 = false;
    has_avx2 = false;
    has_avx512 = false;
    has_neon = true; // Standard on ARM64

    if (has_neon) {
        apply_gain_impl = apply_gain_neon_impl;
        selected_impl = "NEON";
    } else {
        apply_gain_impl = apply_gain_scalar_impl;
        selected_impl = "Scalar";
    }

#else
    // Fallback for other architectures
    has_sse2 = false;
    has_avx2 = false;
    has_avx512 = false;
    has_neon = false;
    apply_gain_impl = apply_gain_scalar_impl;
    selected_impl = "Scalar";
#endif

    features_checked = true;

    SPDLOG_TRACE(
        "Audio gain CPU features - SSE2: {}, AVX2: {}, AVX-512: {}, NEON: {}, Selected: {}",
        has_sse2, has_avx2, has_avx512, has_neon, selected_impl);
}

// Main entry point
void AudioDecoder::apply_gain(uint8_t *buffer, int nb_samples) {
    if (out_samplefmt == AV_SAMPLE_FMT_S16) {
        // Detect CPU features on first call
        detect_cpu_features();

        int16_t *samples = reinterpret_cast<int16_t *>(buffer);
        int total_samples = nb_samples * out_channels;

        // Call the optimized implementation
        apply_gain_impl(samples, total_samples, gain_fixed);
    }
}

// x86_64 SIMD implementations
#if defined(__x86_64__) || defined(_M_X64)

void AudioDecoder::apply_gain_avx512_impl(int16_t *samples, int total_samples,
                                          int gain_fixed) {
#if defined(__AVX10_1__) || defined(__AVX512F__)
    int avx512_end = total_samples & ~31; // Process 32 at once

    __m512i gain_vec = _mm512_set1_epi16(static_cast<int16_t>(gain_fixed));

    for (int i = 0; i < avx512_end; i += 32) {
        __m512i sample_vec =
            _mm512_loadu_si512(reinterpret_cast<__m512i *>(&samples[i]));

        __m512i lo = _mm512_mullo_epi16(sample_vec, gain_vec);
        __m512i hi = _mm512_mulhi_epi16(sample_vec, gain_vec);

        __m512i result_lo = _mm512_unpacklo_epi16(lo, hi);
        __m512i result_hi = _mm512_unpackhi_epi16(lo, hi);

        result_lo = _mm512_srai_epi32(result_lo, 15);
        result_hi = _mm512_srai_epi32(result_hi, 15);

        __m512i final_result = _mm512_packs_epi32(result_lo, result_hi);

        _mm512_storeu_si512(reinterpret_cast<__m512i *>(&samples[i]),
                            final_result);
    }

    // Handle remaining with AVX2
    if (avx512_end < total_samples) {
        apply_gain_avx2_impl(&samples[avx512_end], total_samples - avx512_end,
                             gain_fixed);
    }
#else
    // Fallback if AVX-512 not available at compile time
    apply_gain_avx2_impl(samples, total_samples, gain_fixed);
#endif
}

void AudioDecoder::apply_gain_avx2_impl(int16_t *samples, int total_samples,
                                        int gain_fixed) {
#if defined(__AVX2__)
    int avx2_end = total_samples & ~15; // Process 16 at once

    __m256i gain_vec = _mm256_set1_epi16(static_cast<int16_t>(gain_fixed));

    for (int i = 0; i < avx2_end; i += 16) {
        __m256i sample_vec =
            _mm256_loadu_si256(reinterpret_cast<__m256i *>(&samples[i]));

        __m256i lo = _mm256_mullo_epi16(sample_vec, gain_vec);
        __m256i hi = _mm256_mulhi_epi16(sample_vec, gain_vec);

        __m256i result_lo = _mm256_unpacklo_epi16(lo, hi);
        __m256i result_hi = _mm256_unpackhi_epi16(lo, hi);

        result_lo = _mm256_srai_epi32(result_lo, 15);
        result_hi = _mm256_srai_epi32(result_hi, 15);

        __m256i final_result = _mm256_packs_epi32(result_lo, result_hi);

        _mm256_storeu_si256(reinterpret_cast<__m256i *>(&samples[i]),
                            final_result);
    }

    // Handle remaining with SSE2
    if (avx2_end < total_samples) {
        apply_gain_sse2_impl(&samples[avx2_end], total_samples - avx2_end,
                             gain_fixed);
    }
#else
    // Fallback if AVX2 not available at compile time
    apply_gain_sse2_impl(samples, total_samples, gain_fixed);
#endif
}

void AudioDecoder::apply_gain_sse2_impl(int16_t *samples, int total_samples,
                                        int gain_fixed) {
#if defined(__SSE2__)
    int sse_end = total_samples & ~7; // Process 8 at once

    __m128i gain_vec = _mm_set1_epi16(static_cast<int16_t>(gain_fixed));

    for (int i = 0; i < sse_end; i += 8) {
        __m128i sample_vec =
            _mm_loadu_si128(reinterpret_cast<__m128i *>(&samples[i]));

        __m128i lo = _mm_mullo_epi16(sample_vec, gain_vec);
        __m128i hi = _mm_mulhi_epi16(sample_vec, gain_vec);

        __m128i result_lo = _mm_unpacklo_epi16(lo, hi);
        __m128i result_hi = _mm_unpackhi_epi16(lo, hi);

        result_lo = _mm_srai_epi32(result_lo, 15);
        result_hi = _mm_srai_epi32(result_hi, 15);

        __m128i final_result = _mm_packs_epi32(result_lo, result_hi);

        _mm_storeu_si128(reinterpret_cast<__m128i *>(&samples[i]),
                         final_result);
    }

    // Handle remaining with scalar
    if (sse_end < total_samples) {
        apply_gain_scalar_impl(&samples[sse_end], total_samples - sse_end,
                               gain_fixed);
    }
#else
    // Fallback if SSE2 not available at compile time
    apply_gain_scalar_impl(samples, total_samples, gain_fixed);
#endif
}

#endif // x86_64

// ARM64 NEON implementation
#if defined(__aarch64__) || defined(_M_ARM64)

void AudioDecoder::apply_gain_neon_impl(int16_t *samples, int total_samples,
                                        int gain_fixed) {
#if defined(__ARM_NEON) || defined(_M_ARM64)
    int neon_end = total_samples & ~7; // Process 8 at once

    int16x8_t gain_vec = vdupq_n_s16(static_cast<int16_t>(gain_fixed));

    for (int i = 0; i < neon_end; i += 8) {
        // Load 8 int16 samples
        int16x8_t sample_vec = vld1q_s16(&samples[i]);

        // Multiply to get 32-bit intermediate results
        int32x4_t result_lo =
            vmull_s16(vget_low_s16(sample_vec), vget_low_s16(gain_vec));
        int32x4_t result_hi = vmull_high_s16(sample_vec, gain_vec);

        // Shift right by 15 (divide by 32768)
        result_lo = vshrq_n_s32(result_lo, 15);
        result_hi = vshrq_n_s32(result_hi, 15);

        // Saturate and narrow back to int16
        int16x4_t narrow_lo = vqmovn_s32(result_lo);
        int16x4_t narrow_hi = vqmovn_s32(result_hi);

        // Combine and store
        int16x8_t final_result = vcombine_s16(narrow_lo, narrow_hi);
        vst1q_s16(&samples[i], final_result);
    }

    // Handle remaining samples with scalar
    if (neon_end < total_samples) {
        apply_gain_scalar_impl(&samples[neon_end], total_samples - neon_end,
                               gain_fixed);
    }
#else
    // Fallback if NEON not available at compile time
    apply_gain_scalar_impl(samples, total_samples, gain_fixed);
#endif
}

#endif // aarch64

// Scalar implementation (fallback for all architectures)
void AudioDecoder::apply_gain_scalar_impl(int16_t *samples, int total_samples,
                                          int gain_fixed) {
    for (int i = 0; i < total_samples; ++i) {
        int32_t sample_value =
            (static_cast<int32_t>(samples[i]) * gain_fixed) >> 15;
        sample_value = std::max(-32768, std::min(32767, sample_value));
        samples[i] = static_cast<int16_t>(sample_value);
    }
}

// ==================================================================

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

        int max_dst_nb_samples =
            av_rescale_rnd(frame->nb_samples, out_samplerate,
                           codec_ctx->sample_rate, AV_ROUND_UP);

        AudioDecoderUtils::AVSampleBuffer output_buffer;
        if (!output_buffer.allocate(out_channels, max_dst_nb_samples,
                                    out_samplefmt)) {
            throw std::runtime_error("Failed to allocate output buffer");
        }

        int nb_samples = swr_convert(
            swr_ctx.get(), output_buffer.get_ptr(), max_dst_nb_samples,
            (const uint8_t **)frame->data, frame->nb_samples);
        if (nb_samples < 0) {
            throw std::runtime_error("Error converting samples");
        }
        apply_gain(output_buffer.get(), nb_samples);
        output_stream.write(reinterpret_cast<char *>(output_buffer.get()),
                            nb_samples * out_channels *
                                av_get_bytes_per_sample(out_samplefmt));

        int64_t current_pts =
            frame->pts * av_q2d(fmt_ctx->streams[stream_index]->time_base);
        std::string current_time_str =
            Utilities::format_time(static_cast<int>(current_pts));
        fmt::print("  {:<{}}: {} / {}\r", "position", width, current_time_str,
                   duration_str);
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
    fmt::print("  {:<{}}: {:.3f} ms\n", "took", width, elapsed_time);
}
