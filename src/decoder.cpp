#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

SoxrResampler::SoxrResampler(double inputRate, double outputRate,
                             int outChannels, soxr_datatype_t inType,
                             soxr_datatype_t outType, int quality) {
    logger = spdlog::get("logger");
    soxr_io_spec_t iospec = soxr_io_spec(inType, outType);
    soxr_quality_spec_t q_spec = soxr_quality_spec(quality, 0);
    soxr_error_t error;
    handle = soxr_create(inputRate, outputRate, outChannels, &error, &iospec,
                         &q_spec, nullptr);
    if (!handle) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to create SoXR handle: {}",
                            soxr_strerror(error));
        throw std::runtime_error("");
    }
}

SoxrResampler::~SoxrResampler() {
    soxr_delete(handle);
}

void SoxrResampler::process(const std::vector<int16_t> &audioBuffer,
                            const size_t inputLength,
                            std::vector<int16_t> &resampledBuffer,
                            const size_t outBufferSize, size_t &resampledSize) {
    soxr_error_t error = soxr_process(
        handle, reinterpret_cast<const soxr_in_t *>(audioBuffer.data()),
        inputLength, nullptr,
        reinterpret_cast<soxr_out_t *>(resampledBuffer.data()), outBufferSize,
        &resampledSize);

    if (error) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to process sample: {}",
                            soxr_strerror(error));
        throw std::runtime_error("");
    }
}

BaseDecoder::BaseDecoder(const fs::path &filePath,
                         const std::string_view &pipeName)
    : filePath(filePath), pipeName(pipeName) {
    logger = spdlog::get("logger");
    file_logger = spdlog::get("file_logger");
}

void BaseDecoder::openPipe() {
    pipe.open(pipeName, std::ios::binary);
    if (!pipe) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to open pipe: {}", pipeName);
        throw std::runtime_error("");
    }
}

void BaseDecoder::printDecodingProgress(
    const std::chrono::seconds currentPosition, const std::string &durStr) {
    fmt::print("  {:<{}}: {} / {}\r", "position", WIDTH,
               Utils::formatTime(currentPosition), durStr);
    std::cout.flush();
}

// OpusDecoder implementation
OpusDecoder::OpusDecoder(const fs::path &filePath,
                         const std::string_view &pipeName)
    : BaseDecoder(filePath, pipeName) {}

OpusDecoder::~OpusDecoder() {
    if (opusFile) {
        op_free(opusFile);
    }
}

void OpusDecoder::decode() {
    SPDLOG_LOGGER_INFO(file_logger, "start decode opus");
    constexpr double opusSampleRate = 48000;

    openOpusFile();

    opus_int64 totalSamples = op_pcm_total(opusFile, -1);
    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalSamples / opusSampleRate));

    openPipe();

    std::string durStr = Utils::formatTime(totalDuration);
    readAndWriteOpusData(totalSamples, opusSampleRate, durStr);
    SPDLOG_LOGGER_INFO(file_logger, "finish decode opus");
}

void OpusDecoder::openOpusFile() {
    int error;
    opusFile = op_open_file(filePath.string().data(), &error);
    if (!opusFile) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to open Opus file: {}",
                            filePath.string());
        throw std::runtime_error("");
    }
}

void OpusDecoder::readAndWriteOpusData(opus_int64 totalSamples,
                                       double opusSampleRate,
                                       const std::string &durStr) {
    constexpr int inChannels = 2;
    constexpr size_t bufferSize = 4096;

    std::vector<opus_int16> pcmBuffer(bufferSize * inChannels);
    int samplesRead;

    SPDLOG_LOGGER_INFO(file_logger, "start decode opus loop");
    logger->flush();
    while ((samplesRead =
                op_read_stereo(opusFile, pcmBuffer.data(), bufferSize)) > 0) {
        pipe.write(reinterpret_cast<const char *>(pcmBuffer.data()),
                   samplesRead * inChannels * sizeof(opus_int16));
        if (pipe.fail()) {
            SPDLOG_LOGGER_ERROR(logger, "Error: Failed to write to pipe");
            throw std::runtime_error("");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(op_pcm_tell(opusFile) /
                                          opusSampleRate));
        printDecodingProgress(currentPosition, durStr);
    }
    SPDLOG_LOGGER_INFO(file_logger, "finish decode opus loop");
    logger->flush();

    if (samplesRead < 0) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to decode Opus file");
    }
}

// Mp3Decoder implementation
Mp3Decoder::Mp3Decoder(const fs::path &filePath,
                       const std::string_view &pipeName)
    : BaseDecoder(filePath, pipeName) {}

Mp3Decoder::~Mp3Decoder() {
    if (mpg123Handle) {
        mpg123_close(mpg123Handle);
        mpg123_delete(mpg123Handle);
        mpg123_exit();
    }
}

void Mp3Decoder::decode() {
    SPDLOG_LOGGER_INFO(file_logger, "start decode mp3");
    initializeMpg123();
    createMpg123Handle();
    openMp3File();

    int encoding;
    getMp3Format(inSampleRate, inChannels, encoding);

    encoding = MPG123_ENC_SIGNED_16;
    setMp3Format(outChannels, outSampleRate, encoding);

    openPipe();

    auto totalDuration = getMp3TotalDuration(inSampleRate);
    std::string durStr = Utils::formatTime(totalDuration);

    readResampleAndWriteMp3Data(totalDuration);
    SPDLOG_LOGGER_INFO(file_logger, "finish decode mp3");
}

void Mp3Decoder::initializeMpg123() {
    if (mpg123_init() != MPG123_OK) {
        SPDLOG_LOGGER_ERROR(logger,
                            "Error: Failed to initialize mpg123 library");
        throw std::runtime_error("");
    }
}

void Mp3Decoder::createMpg123Handle() {
    mpg123Handle = mpg123_new(nullptr, nullptr);
    if (!mpg123Handle) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to create mpg123 handle");
        throw std::runtime_error("");
    }
}

void Mp3Decoder::openMp3File() {
    if (mpg123_open(mpg123Handle, filePath.string().data()) != MPG123_OK) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to open MP3 file: {}",
                            filePath.string());
        throw std::runtime_error("");
    }
}

void Mp3Decoder::getMp3Format(long &inSampleRate, int &inChannels,
                              int &encoding) {
    if (mpg123_getformat(mpg123Handle, &inSampleRate, &inChannels, &encoding) !=
        MPG123_OK) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to get MP3 format: {}",
                            mpg123_strerror(mpg123Handle));
        throw std::runtime_error("");
    }
}

void Mp3Decoder::setMp3Format(int outChannels, long outSampleRate,
                              int encoding) {
    if (mpg123_format_none(mpg123Handle) != MPG123_OK ||
        mpg123_format(mpg123Handle, outSampleRate, outChannels, encoding) !=
            MPG123_OK) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to set MP3 format: {}",
                            mpg123_strerror(mpg123Handle));
        throw std::runtime_error("");
    }
}

std::chrono::seconds Mp3Decoder::getMp3TotalDuration(long inSampleRate) {
    off_t totalFrames = mpg123_length(mpg123Handle);
    if (totalFrames == MPG123_ERR) {
        SPDLOG_LOGGER_ERROR(logger,
                            "Error: Failed to get total length of MP3 file: {}",
                            mpg123_strerror(mpg123Handle));
        throw std::runtime_error("");
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalFrames / inSampleRate));
}

void Mp3Decoder::readResampleAndWriteMp3Data(
    std::chrono::seconds totalDuration) {
    constexpr size_t bufferSize = 4096;
    audioBuffer.resize(bufferSize * inChannels);
    size_t bytesRead;

    SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}\n", "i-samplerate", WIDTH,
                       inSampleRate);
    SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}\n", "o-samplerate", WIDTH,
                       outSampleRate);

    SoxrResampler soxrResampler(inSampleRate, outSampleRate, outChannels,
                                SOXR_INT16_I, SOXR_INT16_I, SOXR_HQ);
    bool resample =
        (outSampleRate != inSampleRate || inChannels != outChannels);
    if (resample) {
        SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {} -> {}, {} -> {}", "resample",
                           WIDTH, inSampleRate, outSampleRate, inChannels,
                           outChannels);
        double freqRatio = outSampleRate / static_cast<double>(inSampleRate);
        outBufferSize = static_cast<size_t>(bufferSize * freqRatio + 1.0);
        resampledBuffer.resize(outBufferSize * outChannels);
    }

    int err;
    std::string durStr = Utils::formatTime(totalDuration);

    SPDLOG_LOGGER_INFO(file_logger, "start decode mp3 loop");
    logger->flush();
    while ((err = mpg123_read(mpg123Handle, audioBuffer.data(),
                              bufferSize * inChannels * sizeof(int16_t),
                              &bytesRead)) == MPG123_OK) {
        processWriteData(resample, soxrResampler, bytesRead);

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(mpg123_tell(mpg123Handle) /
                                          inSampleRate));
        printDecodingProgress(currentPosition, durStr);
    }
    SPDLOG_LOGGER_INFO(file_logger, "finish decode mp3 loop");
    logger->flush();

    if (err != MPG123_DONE) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to decode MP3 file: {}",
                            mpg123_strerror(mpg123Handle));
    }
}

void Mp3Decoder::processWriteData(bool resample, SoxrResampler &soxrResampler,
                                  size_t bytesRead) {
    if (resample) {
        size_t resampledSize;
        size_t inputLength = (bytesRead / sizeof(int16_t)) / inChannels;
        soxrResampler.process(audioBuffer, inputLength, resampledBuffer,
                              outBufferSize, resampledSize);
        pipe.write(reinterpret_cast<const char *>(resampledBuffer.data()),
                   resampledSize * outChannels * sizeof(int16_t));
    } else {
        pipe.write(reinterpret_cast<const char *>(audioBuffer.data()),
                   bytesRead);
    }

    if (pipe.fail()) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to write to pipe");
        throw std::runtime_error("");
    }
}

Decoder::Decoder(const fs::path &filePath, const std::string_view &extension,
                 const std::string_view &pipeName)
    : BaseDecoder(filePath, pipeName), filePath(filePath), extension(extension),
      pipeName(pipeName) {
    if (extension == "opus") {
        decoder = std::make_unique<OpusDecoder>(filePath, pipeName);
    } else if (extension == "mp3") {
        decoder = std::make_unique<Mp3Decoder>(filePath, pipeName);
    } else {
        SPDLOG_LOGGER_ERROR(logger, "Error: Unsupported format: {}",
                            filePath.string());
        throw std::runtime_error("");
    }
}

void Decoder::printMetadata() {
    TagLib::FileRef f(filePath.string().data());
    if (f.isNull()) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Invalid or unsupported file: {}",
                            filePath.string());
        return;
    }

    if (auto *tag = f.tag()) {
        printTag("title", tag->title());
        printTag("artist", tag->artist());
        printTag("album", tag->album());
        if (tag->year() != 0) {
            SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "year", WIDTH,
                               tag->year());
        }
        printTag("comment", tag->comment());
        if (tag->track() != 0) {
            SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "track", WIDTH,
                               tag->track());
        }
        printTag("genre", tag->genre());
    }

    printAudioProperties(f.audioProperties());
    printFileProperties(f.file()->properties());
    logger->flush();
}

void Decoder::printTag(const std::string_view name,
                       const TagLib::String &value) {
    if (!value.isEmpty()) {
        SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", name, WIDTH,
                           value.stripWhiteSpace().to8Bit(true));
    }
}

void Decoder::printAudioProperties(const TagLib::AudioProperties *properties) {
    if (properties) {
        if (properties->bitrate() != 0) {
            SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {} kbps", "bitrate", WIDTH,
                               properties->bitrate());
        }
        if (properties->sampleRate() != 0) {
            SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "sample-rate", WIDTH,
                               properties->sampleRate());
        }
        if (properties->channels() != 0) {
            SPDLOG_LOGGER_INFO(logger, "  {:<{}}: {}", "channels", WIDTH,
                               properties->channels());
        }
        if (properties->lengthInMilliseconds() != 0) {
            auto length =
                std::chrono::milliseconds(properties->lengthInMilliseconds());
            SPDLOG_LOGGER_INFO(
                logger, "  {:<{}}: {}", "length", WIDTH,
                Utils::formatTime(
                    std::chrono::duration_cast<std::chrono::seconds>(length)));
        }
    }
}

void Decoder::printFileProperties(const TagLib::PropertyMap &fileProperties) {
    for (const auto &[key, values] : fileProperties) {
        if (!key.isEmpty() && !values.isEmpty()) {
            std::string keyStr = key.stripWhiteSpace().to8Bit(true);
            Utils::toLowercase(keyStr);
            std::string kv = fmt::format("  {:<{}}:", keyStr, WIDTH);
            for (const auto &value : values) {
                if (!value.isEmpty()) {
                    kv = fmt::format("  {} {}", kv,
                                     value.stripWhiteSpace().to8Bit(true));
                }
            }
            SPDLOG_LOGGER_INFO(logger, "{}", kv);
        }
    }
}

void Decoder::decode() {
    SPDLOG_LOGGER_INFO(file_logger, "start decode");
    if (decoder) {
        decoder->decode();
    } else {
        SPDLOG_LOGGER_ERROR(logger, "Error: Decoder not initialized");
        throw std::runtime_error("");
    }
    fmt::print("\n\n");
    SPDLOG_LOGGER_INFO(file_logger, "finish decode");
}
