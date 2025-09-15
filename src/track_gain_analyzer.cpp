#include "track_gain_analyzer.hpp"

#include <cstdarg>
#include <stdexcept>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

double TrackGainAnalyzer::compute_and_write_track_gain(
    const std::filesystem::path &file_path) {
    SPDLOG_TRACE("Computing and writing track gain for: {}",
                 file_path.string());

    TrackGainAnalyzer analyzer(file_path);
    double gain = analyzer.compute_track_gain();
    analyzer.write_track_gain_tag(gain);

    SPDLOG_TRACE("Track gain computed: {:.2f} dB", gain);
    return gain;
}

TrackGainAnalyzer::TrackGainAnalyzer(const std::filesystem::path &file_path)
    : file_path(file_path), audio_stream_index(-1) {
    SPDLOG_TRACE("Initializing TrackGainAnalyzer for: {}", file_path.string());
    setup_ffmpeg_logging();
}

double TrackGainAnalyzer::compute_track_gain() {
    SPDLOG_TRACE("Starting track gain computation");

    initialize_ffmpeg();
    find_audio_stream();
    initialize_codec();
    initialize_resampler();
    initialize_ebur128();
    process_audio_data();

    double loudness;
    int result = ebur128_loudness_global(ebu_state.get(), &loudness);
    if (result != EBUR128_SUCCESS) {
        throw std::runtime_error(
            fmt::format("Failed to compute loudness: error code {}", result));
    }

    // Calculate track gain: reference_loudness - measured_loudness
    double track_gain = reference_loudness - loudness;

    SPDLOG_TRACE("Measured loudness: {:.2f} LUFS, Track gain: {:.2f} dB",
                 loudness, track_gain);
    return track_gain;
}

void TrackGainAnalyzer::write_track_gain_tag(double gain_value) {
    // dB
    std::string gain_string = fmt::format("{:.2f}", gain_value);
    SPDLOG_TRACE("Writing R128_TRACK_GAIN tag: {}", gain_string);
    write_metadata_tag("R128_TRACK_GAIN", gain_string);
}

void TrackGainAnalyzer::initialize_ffmpeg() {
    SPDLOG_TRACE("Initializing FFmpeg");

    AVFormatContext *tmp_format_ctx = nullptr;

    if (avformat_open_input(&tmp_format_ctx, file_path.c_str(), nullptr,
                            nullptr) < 0) {
        throw std::runtime_error(
            fmt::format("Failed to open input file: {}", file_path.string()));
    }

    format_context = FormatContextPtr(tmp_format_ctx);

    if (avformat_find_stream_info(format_context.get(), nullptr) < 0) {
        throw std::runtime_error("Failed to find stream information");
    }

    SPDLOG_TRACE("FFmpeg initialized successfully");
}

void TrackGainAnalyzer::find_audio_stream() {
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

void TrackGainAnalyzer::initialize_codec() {
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

    codec_context = CodecContextPtr(tmp_codec_ctx);

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
    packet = PacketPtr(tmp_pkt);

    AVFrame *tmp_frame = av_frame_alloc();
    if (!tmp_frame) {
        throw std::runtime_error("Failed to allocate frame");
    }
    frame = FramePtr(tmp_frame);

    SPDLOG_TRACE("Codec initialized: {} ({})", codec->long_name, codec->name);
}

void TrackGainAnalyzer::initialize_resampler() {
    SPDLOG_TRACE("Initializing resampler");

    SwrContext *tmp_swr = swr_alloc();
    if (!tmp_swr) {
        throw std::runtime_error("Failed to allocate resampler");
    }

    swr_context = SwrContextPtr(tmp_swr);

    // Set input parameters
    av_opt_set_chlayout(swr_context.get(), "in_chlayout",
                        &codec_context->ch_layout, 0);
    av_opt_set_int(swr_context.get(), "in_sample_rate",
                   codec_context->sample_rate, 0);
    av_opt_set_sample_fmt(swr_context.get(), "in_sample_fmt",
                          codec_context->sample_fmt, 0);

    // Set output parameters
    AVChannelLayout out_chlayout;
    av_channel_layout_default(&out_chlayout, target_channels);
    av_opt_set_chlayout(swr_context.get(), "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(swr_context.get(), "out_sample_rate", target_sample_rate, 0);
    av_opt_set_sample_fmt(swr_context.get(), "out_sample_fmt",
                          target_sample_format, 0);

    // Use high-quality resampler
    av_opt_set(swr_context.get(), "resampler", "soxr", 0);
    av_opt_set_int(swr_context.get(), "precision", 20, 0);

    if (swr_init(swr_context.get()) < 0) {
        throw std::runtime_error("Failed to initialize resampler");
    }

    SPDLOG_TRACE("Resampler initialized: {} Hz {} ch -> {} Hz {} ch",
                 codec_context->sample_rate,
                 codec_context->ch_layout.nb_channels, target_sample_rate,
                 target_channels);
}

void TrackGainAnalyzer::initialize_ebur128() {
    SPDLOG_TRACE("Initializing EBU R128 state");

    ebur128_state *tmp_state =
        ebur128_init(target_channels, target_sample_rate, EBUR128_MODE_I);
    if (!tmp_state) {
        throw std::runtime_error("Failed to initialize EBU R128 state");
    }

    ebu_state = EbuStatePtr(tmp_state);

    SPDLOG_TRACE("EBU R128 state initialized for {} channels at {} Hz",
                 target_channels, target_sample_rate);
}

void TrackGainAnalyzer::process_audio_data() {
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

                int max_dst_nb_samples =
                    av_rescale_rnd(frame->nb_samples, target_sample_rate,
                                   codec_context->sample_rate, AV_ROUND_UP);

                SampleBuffer output_buffer;
                if (!output_buffer.allocate(target_channels, max_dst_nb_samples,
                                            target_sample_format)) {
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

                // Add samples to EBU R128 analyzer
                int result = ebur128_add_frames_double(
                    ebu_state.get(),
                    reinterpret_cast<double *>(output_buffer.get()),
                    nb_samples);
                if (result != EBUR128_SUCCESS) {
                    throw std::runtime_error(fmt::format(
                        "Failed to add frames to EBU R128: error code {}",
                        result));
                }

                frames_processed++;
            }
        }
        av_packet_unref(packet.get());
    }

    SPDLOG_TRACE("Audio data processing completed, processed {} frames",
                 frames_processed);
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

    // Get the property map for advanced tag writing
    TagLib::PropertyMap properties = file_ref.file()->properties();

    // Add the R128_TRACK_GAIN property
    TagLib::StringList gain_values;
    gain_values.append(TagLib::String(value.c_str()));
    properties.replace(TagLib::String(key.c_str()), gain_values);

    // Write the properties back to the file
    TagLib::PropertyMap unsuccessful =
        file_ref.file()->setProperties(properties);

    if (!unsuccessful.isEmpty()) {
        SPDLOG_TRACE("Some properties could not be written: {}",
                     unsuccessful.size());
        // Log which properties failed (optional)
        for (const auto &prop : unsuccessful) {
            SPDLOG_TRACE("Failed to write property: {}",
                         prop.first.toCString());
        }
    }

    // Save the file
    if (!file_ref.save()) {
        throw std::runtime_error("Failed to save metadata tag to file");
    }

    SPDLOG_TRACE("Metadata tag written successfully using TagLib");
}

void TrackGainAnalyzer::setup_ffmpeg_logging() {
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback([](void *avcl, int level, const char *fmt, va_list vl) {
        if (level <= av_log_get_level()) {
            char buffer[1024];
            vsnprintf(buffer, sizeof(buffer), fmt, vl);
            SPDLOG_TRACE("FFmpeg: {}", buffer);
        }
    });
}