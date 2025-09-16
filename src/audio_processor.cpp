#include <chrono>
#include <cstdarg>
#include <stdexcept>

#include <fmt/core.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include "audio_processor.hpp"
#include "utils.hpp"

// SIMD headers
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

// =============================================================================
// AudioProcessorBase Implementation
// =============================================================================

AudioProcessorBase::AudioProcessorBase(const fs::path &file_path)
    : file_path(file_path), audio_config{} {
    setup_ffmpeg_logging();
}

void AudioProcessorBase::setup_ffmpeg_logging() {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log_callback);
}

void AudioProcessorBase::ffmpeg_log_callback(void *avcl, int level,
                                             const char *fmt, va_list vl) {
    if (level <= av_log_get_level()) {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, vl);
        SPDLOG_TRACE("FFmpeg: {}", buffer);
    }
}

void AudioProcessorBase::initialize_ffmpeg() {
    SPDLOG_TRACE("Initializing FFmpeg for: {}", file_path.string());

    AVFormatContext *tmp_format_ctx = nullptr;
    if (avformat_open_input(&tmp_format_ctx, file_path.c_str(), nullptr,
                            nullptr) < 0) {
        throw std::runtime_error(
            fmt::format("Failed to open input file: {}", file_path.string()));
    }
    format_context = AudioUtils::AVFormatContextPtr(tmp_format_ctx);

    if (avformat_find_stream_info(format_context.get(), nullptr) < 0) {
        throw std::runtime_error("Failed to find stream information");
    }

    SPDLOG_TRACE("FFmpeg initialized successfully");
}

void AudioProcessorBase::find_audio_stream() {
    SPDLOG_TRACE("Finding audio stream");

    for (unsigned int i = 0; i < format_context->nb_streams; ++i) {
        if (format_context->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = static_cast<int>(i);
            SPDLOG_TRACE("Found audio stream at index: {}", audio_stream_index);
            return;
        }
    }

    throw std::runtime_error("No audio stream found in file");
}

void AudioProcessorBase::initialize_codec() {
    SPDLOG_TRACE("Initializing codec");

    const AVCodec *codec = avcodec_find_decoder(
        format_context->streams[audio_stream_index]->codecpar->codec_id);
    if (!codec) {
        throw std::runtime_error("Failed to find decoder");
    }

    AVCodecContext *tmp_codec_ctx = avcodec_alloc_context3(codec);
    if (!tmp_codec_ctx) {
        throw std::runtime_error("Failed to allocate codec context");
    }
    codec_context = AudioUtils::AVCodecContextPtr(tmp_codec_ctx);

    if (avcodec_parameters_to_context(
            codec_context.get(),
            format_context->streams[audio_stream_index]->codecpar) < 0) {
        throw std::runtime_error("Failed to copy codec parameters");
    }

    codec_context->pkt_timebase =
        format_context->streams[audio_stream_index]->time_base;

    if (avcodec_open2(codec_context.get(), codec, nullptr) < 0) {
        throw std::runtime_error("Failed to open codec");
    }

    // Allocate packet and frame
    AVPacket *tmp_pkt = av_packet_alloc();
    if (!tmp_pkt) {
        throw std::runtime_error("Failed to allocate packet");
    }
    packet = AudioUtils::AVPacketPtr(tmp_pkt);

    AVFrame *tmp_frame = av_frame_alloc();
    if (!tmp_frame) {
        throw std::runtime_error("Failed to allocate frame");
    }
    frame = AudioUtils::AVFramePtr(tmp_frame);

    SPDLOG_TRACE("Codec initialized: {} ({})", codec->long_name, codec->name);
}

void AudioProcessorBase::initialize_resampler() {
    SPDLOG_TRACE("Initializing resampler");

    SwrContext *tmp_swr = swr_alloc();
    if (!tmp_swr) {
        throw std::runtime_error("Failed to allocate resampler");
    }
    swr_context = AudioUtils::SwrContextPtr(tmp_swr);

    // Set input parameters
    av_opt_set_chlayout(swr_context.get(), "in_chlayout",
                        &codec_context->ch_layout, 0);
    av_opt_set_int(swr_context.get(), "in_sample_rate",
                   codec_context->sample_rate, 0);
    av_opt_set_sample_fmt(swr_context.get(), "in_sample_fmt",
                          codec_context->sample_fmt, 0);

    // Set output parameters
    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, audio_config.channels);
    av_opt_set_chlayout(swr_context.get(), "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_context.get(), "out_sample_rate",
                   audio_config.sample_rate, 0);
    av_opt_set_sample_fmt(swr_context.get(), "out_sample_fmt",
                          audio_config.sample_format, 0);

    // Set quality parameters
    av_opt_set(swr_context.get(), "resampler", audio_config.resampler.c_str(),
               0);
    av_opt_set_int(swr_context.get(), "precision", audio_config.precision, 0);

    if (swr_init(swr_context.get()) < 0) {
        throw std::runtime_error("Failed to initialize resampler");
    }

    SPDLOG_TRACE("Resampler initialized: {} Hz {} ch -> {} Hz {} ch",
                 codec_context->sample_rate,
                 codec_context->ch_layout.nb_channels, audio_config.sample_rate,
                 audio_config.channels);
}

void AudioProcessorBase::process_audio_data(
    const AudioUtils::AudioSampleCallback &callback) {
    SPDLOG_TRACE("Starting audio data processing");

    int frames_processed = 0;

    while (av_read_frame(format_context.get(), packet.get()) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            int ret = avcodec_send_packet(codec_context.get(), packet.get());
            if (ret < 0) {
                throw std::runtime_error("Failed to send packet to decoder");
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_context.get(), frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    throw std::runtime_error("Error during decoding");
                }

                // Calculate output buffer size for resampling
                int max_dst_nb_samples =
                    av_rescale_rnd(frame->nb_samples, audio_config.sample_rate,
                                   codec_context->sample_rate, AV_ROUND_UP);

                AudioUtils::SampleBuffer output_buffer;
                if (!output_buffer.allocate(audio_config.channels,
                                            max_dst_nb_samples,
                                            audio_config.sample_format)) {
                    throw std::runtime_error(
                        "Failed to allocate output buffer");
                }

                int nb_samples =
                    swr_convert(swr_context.get(), output_buffer.get_ptr(),
                                max_dst_nb_samples,
                                const_cast<const uint8_t **>(frame->data),
                                frame->nb_samples);

                if (nb_samples < 0) {
                    throw std::runtime_error("Error converting samples");
                }

                // Call the callback with resampled samples
                callback(output_buffer.get(), nb_samples,
                         audio_config.channels);
                frames_processed++;
            }
        }
        av_packet_unref(packet.get());
    }

    SPDLOG_TRACE("Audio data processing completed, processed {} frames",
                 frames_processed);
}

void AudioProcessorBase::print_metadata() const {
    AVDictionaryEntry *tag = nullptr;
    constexpr int width = 16;

    while ((tag = av_dict_get(format_context->metadata, "", tag,
                              AV_DICT_IGNORE_SUFFIX))) {
        std::string key_str(tag->key);
        Utilities::to_lower(key_str);
        fmt::print("  {:<{}}: {}\n", key_str, width, tag->value);
    }

    for (unsigned i = 0; i < format_context->nb_streams; i++) {
        AVStream *stream = format_context->streams[i];
        while ((tag = av_dict_get(stream->metadata, "", tag,
                                  AV_DICT_IGNORE_SUFFIX))) {
            std::string key_str(tag->key);
            Utilities::to_lower(key_str);
            fmt::print("  {:<{}}: {}\n", key_str, width, tag->value);
        }
    }
}

void AudioProcessorBase::print_audio_info(
    const AudioUtils::AudioConfig &config) const {
    constexpr int width = 16;

    fmt::print("  {:<{}}: {}\n", "codec", width,
               codec_context->codec->long_name);
    if (codec_context->bit_rate != 0) {
        fmt::print("  {:<{}}: {} kbps\n", "bit-rate", width,
                   codec_context->bit_rate / 1000);
    }
    fmt::print("  {:<{}}: {}\n", "sample-rate", width,
               codec_context->sample_rate);

    char sample_fmt_str[16];
    av_get_sample_fmt_string(sample_fmt_str, sizeof(sample_fmt_str),
                             codec_context->sample_fmt);
    std::string fmt_str(sample_fmt_str);
    Utilities::trim_spaces(fmt_str);
    fmt::print("  {:<{}}: {}\n", "sample-fmt", width, fmt_str);
    fmt::print("  {:<{}}: {}\n", "channels", width,
               codec_context->ch_layout.nb_channels);

    if (codec_context->ch_layout.nb_channels != config.channels) {
        fmt::print("  {:<{}}: {} -> {}\n", "resample-channels", width,
                   codec_context->ch_layout.nb_channels, config.channels);
    }
    if (codec_context->sample_rate != config.sample_rate) {
        fmt::print("  {:<{}}: {} -> {}\n", "resample-rate", width,
                   codec_context->sample_rate, config.sample_rate);
    }
}

int64_t AudioProcessorBase::get_duration() const {
    if (format_context->streams[audio_stream_index]->duration !=
        AV_NOPTS_VALUE) {
        return format_context->streams[audio_stream_index]->duration *
               av_q2d(format_context->streams[audio_stream_index]->time_base);
    } else if (format_context->duration != AV_NOPTS_VALUE) {
        return format_context->duration / AV_TIME_BASE;
    }
    return -1;
}

// =============================================================================
// GainProcessor Implementation
// =============================================================================

bool GainProcessor::features_checked = false;
bool GainProcessor::has_avx512 = false;
bool GainProcessor::has_avx2 = false;
bool GainProcessor::has_sse2 = false;
bool GainProcessor::has_neon = false;
void (*GainProcessor::apply_gain_impl)(int16_t *, int, int) = nullptr;

GainProcessor::GainProcessor(int gain_fixed) : gain_fixed(gain_fixed) {}

void GainProcessor::detect_cpu_features() {
    if (features_checked)
        return;

    const char *selected_impl = "unknown";

#if defined(__x86_64__) || defined(_M_X64)
    has_sse2 = __builtin_cpu_supports("sse2");
    has_avx2 = __builtin_cpu_supports("avx2");
    has_avx512 = __builtin_cpu_supports("avx512f");

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
    has_neon = true; // Standard on ARM64
    if (has_neon) {
        apply_gain_impl = apply_gain_neon_impl;
        selected_impl = "NEON";
    } else {
        apply_gain_impl = apply_gain_scalar_impl;
        selected_impl = "Scalar";
    }

#else
    apply_gain_impl = apply_gain_scalar_impl;
    selected_impl = "Scalar";
#endif

    features_checked = true;
    SPDLOG_TRACE(
        "Audio gain CPU features - SSE2: {}, AVX2: {}, AVX-512: {}, NEON: {}, Selected: {}",
        has_sse2, has_avx2, has_avx512, has_neon, selected_impl);
}

void GainProcessor::apply_gain(uint8_t *buffer, int nb_samples, int channels) {
    detect_cpu_features();

    int16_t *samples = reinterpret_cast<int16_t *>(buffer);
    int total_samples = nb_samples * channels;
    apply_gain_impl(samples, total_samples, gain_fixed);
}

// SIMD implementations (same as original but moved to GainProcessor class)
#if defined(__x86_64__) || defined(_M_X64)

void GainProcessor::apply_gain_avx512_impl(int16_t *samples, int total_samples,
                                           int gain_fixed) {
#if defined(__AVX10_1__) || defined(__AVX512F__)
    int avx512_end = total_samples & ~31;
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

    if (avx512_end < total_samples) {
        apply_gain_avx2_impl(&samples[avx512_end], total_samples - avx512_end,
                             gain_fixed);
    }
#else
    apply_gain_avx2_impl(samples, total_samples, gain_fixed);
#endif
}

void GainProcessor::apply_gain_avx2_impl(int16_t *samples, int total_samples,
                                         int gain_fixed) {
#if defined(__AVX2__)
    int avx2_end = total_samples & ~15;
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

    if (avx2_end < total_samples) {
        apply_gain_sse2_impl(&samples[avx2_end], total_samples - avx2_end,
                             gain_fixed);
    }
#else
    apply_gain_sse2_impl(samples, total_samples, gain_fixed);
#endif
}

void GainProcessor::apply_gain_sse2_impl(int16_t *samples, int total_samples,
                                         int gain_fixed) {
#if defined(__SSE2__)
    int sse_end = total_samples & ~7;
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

    if (sse_end < total_samples) {
        apply_gain_scalar_impl(&samples[sse_end], total_samples - sse_end,
                               gain_fixed);
    }
#else
    apply_gain_scalar_impl(samples, total_samples, gain_fixed);
#endif
}

#endif // x86_64

#if defined(__aarch64__) || defined(_M_ARM64)

void GainProcessor::apply_gain_neon_impl(int16_t *samples, int total_samples,
                                         int gain_fixed) {
#if defined(__ARM_NEON) || defined(_M_ARM64)
    int neon_end = total_samples & ~7;
    int16x8_t gain_vec = vdupq_n_s16(static_cast<int16_t>(gain_fixed));

    for (int i = 0; i < neon_end; i += 8) {
        int16x8_t sample_vec = vld1q_s16(&samples[i]);
        int32x4_t result_lo =
            vmull_s16(vget_low_s16(sample_vec), vget_low_s16(gain_vec));
        int32x4_t result_hi = vmull_high_s16(sample_vec, gain_vec);
        result_lo = vshrq_n_s32(result_lo, 15);
        result_hi = vshrq_n_s32(result_hi, 15);
        int16x4_t narrow_lo = vqmovn_s32(result_lo);
        int16x4_t narrow_hi = vqmovn_s32(result_hi);
        int16x8_t final_result = vcombine_s16(narrow_lo, narrow_hi);
        vst1q_s16(&samples[i], final_result);
    }

    if (neon_end < total_samples) {
        apply_gain_scalar_impl(&samples[neon_end], total_samples - neon_end,
                               gain_fixed);
    }
#else
    apply_gain_scalar_impl(samples, total_samples, gain_fixed);
#endif
}

#endif // aarch64

void GainProcessor::apply_gain_scalar_impl(int16_t *samples, int total_samples,
                                           int gain_fixed) {
    for (int i = 0; i < total_samples; ++i) {
        int32_t sample_value =
            (static_cast<int32_t>(samples[i]) * gain_fixed) >> 15;
        sample_value = std::max(-32768, std::min(32767, sample_value));
        samples[i] = static_cast<int16_t>(sample_value);
    }
}

// =============================================================================
// AudioDecoder Implementation
// =============================================================================

AudioDecoder::AudioDecoder(const std::string &pipe_name,
                           const fs::path &file_path, int gain_fixed)
    : AudioProcessorBase(file_path), pipe_name(pipe_name),
      gain_processor(gain_fixed) {
    audio_config = OUTPUT_CONFIG;
}

void AudioDecoder::decode() {
    SPDLOG_TRACE("Start decoding {}", file_path.filename().string());
    auto start = std::chrono::steady_clock::now();

    try {
        initialize();
    } catch (const std::runtime_error &e) {
        SPDLOG_TRACE("init failed: {}", e.what());
        return;
    }

    log_duration(start);

    SPDLOG_TRACE("Start decode loop");

    // Create callback for processing audio samples
    auto process_samples = [this](uint8_t *buffer, int nb_samples,
                                  int channels) {
        // Apply gain processing
        gain_processor.apply_gain(buffer, nb_samples, channels);

        // Write to output stream
        int bytes_per_sample =
            av_get_bytes_per_sample(audio_config.sample_format);
        output_stream.write(reinterpret_cast<char *>(buffer),
                            nb_samples * channels * bytes_per_sample);

        // Update progress display
        int64_t current_pts =
            frame->pts *
            av_q2d(format_context->streams[audio_stream_index]->time_base);
        std::string current_time_str =
            Utilities::format_time(static_cast<int>(current_pts));
        fmt::print("  {:<{}}: {} / {}\r", "position", 16, current_time_str,
                   duration_str);
        std::cout.flush();
    };

    process_audio_data(process_samples);

    fmt::print("\n\n");
    SPDLOG_TRACE("Finish decode loop");
    SPDLOG_TRACE("End decoding {}", file_path.filename().string());
}

void AudioDecoder::initialize() {
    initialize_ffmpeg();
    print_metadata();
    find_audio_stream();
    initialize_codec();
    initialize_resampler();
    open_output_pipe();

    int64_t duration = get_duration();
    duration_str = Utilities::format_time(static_cast<int>(duration));
    print_audio_info(audio_config);
}

void AudioDecoder::open_output_pipe() {
    output_stream.open(pipe_name, std::ios::binary);
    if (!output_stream.is_open()) {
        throw std::runtime_error("Failed to open output pipe");
    }
}

void AudioDecoder::log_duration(
    std::chrono::steady_clock::time_point start) const {
    auto end = std::chrono::steady_clock::now();
    double elapsed_time =
        std::chrono::duration<double, std::milli>(end - start).count();
    fmt::print("  {:<{}}: {:.3f} ms\n", "took", 16, elapsed_time);
}

// =============================================================================
// TrackGainAnalyzer Implementation
// =============================================================================

double
TrackGainAnalyzer::compute_and_write_track_gain(const fs::path &file_path) {
    SPDLOG_TRACE("Computing and writing track gain for: {}",
                 file_path.string());
    auto start = std::chrono::steady_clock::now();

    TrackGainAnalyzer analyzer(file_path);
    double gain = analyzer.compute_track_gain();
    analyzer.write_track_gain_tag(gain);

    auto end = std::chrono::steady_clock::now();
    double elapsed_time = std::chrono::duration<double>(end - start).count();
    SPDLOG_TRACE("Track gain analysis completed in {:.3f} s: {:.2f} dB",
                 elapsed_time, gain);
    return gain;
}

TrackGainAnalyzer::TrackGainAnalyzer(const fs::path &file_path)
    : AudioProcessorBase(file_path) {
    SPDLOG_TRACE("Initializing TrackGainAnalyzer for: {}", file_path.string());
    audio_config = ANALYSIS_CONFIG;
}

double TrackGainAnalyzer::compute_track_gain() {
    SPDLOG_TRACE("Starting track gain computation");
    auto start = std::chrono::steady_clock::now();

    initialize_ffmpeg();
    find_audio_stream();
    initialize_codec();
    initialize_resampler();
    initialize_ebur128();

    // Create callback for EBU R128 analysis
    auto analyze_samples = [this](uint8_t *buffer, int nb_samples,
                                  int channels) {
        int result = ebur128_add_frames_double(
            ebu_state.get(), reinterpret_cast<double *>(buffer), nb_samples);
        if (result != EBUR128_SUCCESS) {
            throw std::runtime_error(fmt::format(
                "Failed to add frames to EBU R128: error code {}", result));
        }
    };

    process_audio_data(analyze_samples);

    double loudness;
    int result = ebur128_loudness_global(ebu_state.get(), &loudness);
    if (result != EBUR128_SUCCESS) {
        throw std::runtime_error(
            fmt::format("Failed to compute loudness: error code {}", result));
    }

    double track_gain = REFERENCE_LOUDNESS - loudness;

    auto end = std::chrono::steady_clock::now();
    double elapsed_time = std::chrono::duration<double>(end - start).count();

    SPDLOG_TRACE("Track gain computation completed in {:.3f} s", elapsed_time);
    SPDLOG_TRACE("Measured loudness: {:.2f} LUFS, Track gain: {:.2f} dB",
                 loudness, track_gain);

    return track_gain;
}

void TrackGainAnalyzer::write_track_gain_tag(double gain_value) {
    std::string gain_string = fmt::format("{:.2f}", gain_value);
    SPDLOG_TRACE("Writing R128_TRACK_GAIN tag: {}", gain_string);
    write_metadata_tag("R128_TRACK_GAIN", gain_string);
}

void TrackGainAnalyzer::initialize_ebur128() {
    SPDLOG_TRACE("Initializing EBU R128 state");

    ebur128_state *tmp_state = ebur128_init(
        audio_config.channels, audio_config.sample_rate, EBUR128_MODE_I);
    if (!tmp_state) {
        throw std::runtime_error("Failed to initialize EBU R128 state");
    }

    ebu_state = EbuStatePtr(tmp_state);
    SPDLOG_TRACE("EBU R128 state initialized for {} channels at {} Hz",
                 audio_config.channels, audio_config.sample_rate);
}

void TrackGainAnalyzer::write_metadata_tag(const std::string &key,
                                           const std::string &value) {
    SPDLOG_TRACE("Writing metadata tag using TagLib: {} = {}", key, value);

    TagLib::FileRef file_ref(file_path.c_str());
    if (file_ref.isNull()) {
        throw std::runtime_error(fmt::format(
            "Failed to open file with TagLib: {}", file_path.string()));
    }

    if (!file_ref.tag()) {
        throw std::runtime_error("File has no tag support");
    }

    TagLib::PropertyMap properties = file_ref.file()->properties();
    TagLib::StringList gain_values;
    gain_values.append(TagLib::String(value.c_str()));
    properties.replace(TagLib::String(key.c_str()), gain_values);

    TagLib::PropertyMap unsuccessful =
        file_ref.file()->setProperties(properties);
    if (!unsuccessful.isEmpty()) {
        SPDLOG_TRACE("Some properties could not be written: {}",
                     unsuccessful.size());
        for (const auto &prop : unsuccessful) {
            SPDLOG_TRACE("Failed to write property: {}",
                         prop.first.toCString());
        }
    }

    if (!file_ref.save()) {
        throw std::runtime_error("Failed to save metadata tag to file");
    }

    SPDLOG_TRACE("Metadata tag written successfully using TagLib");
}