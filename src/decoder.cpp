#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mpg123.h>
#include <opusfile.h>
#include <stdexcept>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <vector>

namespace fs = std::filesystem;

Decoder::Decoder(const std::filesystem::path &filePath,
                 std::string_view extension,
                 std::string_view pipeName)
    : filePath(filePath), extension(extension), pipeName(pipeName) {}

void Decoder::decode() {
    if (extension == "mp3") {
        decodeMp3();
    } else if (extension == "opus") {
        decodeOpus();
    } else {
        fmt::print("  {:<{}} : {}", "error", WIDTH, "Unsupported format");
    }
    fmt::print("\n\n");
}

void Decoder::printMetadata() {
    TagLib::FileRef f(filePath.string().data());
    if (f.isNull()) {
        fmt::print(
            "  {:<{}} : {}\n", "error", WIDTH, "Invalid or unsupported file");
        return;
    }

    auto printTag = [](std::string_view name, const TagLib::String &value) {
        if (!value.isEmpty()) {
            fmt::print("  {:<{}} : {}\n",
                       name,
                       WIDTH,
                       value.stripWhiteSpace().to8Bit(true));
        }
    };

    if (auto *tag = f.tag()) {
        printTag("title", tag->title());
        printTag("artist", tag->artist());
        printTag("album", tag->album());
        if (tag->year() != 0) {
            fmt::print("  {:<{}} : {}\n", "year", WIDTH, tag->year());
        }
        printTag("comment", tag->comment());
        if (tag->track() != 0) {
            fmt::print("  {:<{}} : {}\n", "track", WIDTH, tag->track());
        }
        printTag("genre", tag->genre());
    }

    if (auto *properties = f.audioProperties()) {
        if (properties->bitrate() != 0) {
            fmt::print("  {:<{}} : {} kbps\n",
                       "bitrate",
                       WIDTH,
                       properties->bitrate());
        }
        if (properties->sampleRate() != 0) {
            fmt::print("  {:<{}} : {} Hz\n",
                       "sample-rate",
                       WIDTH,
                       properties->sampleRate());
        }
        if (properties->channels() != 0) {
            fmt::print(
                "  {:<{}} : {}\n", "channels", WIDTH, properties->channels());
        }
        if (properties->lengthInMilliseconds() != 0) {
            auto length =
                std::chrono::milliseconds(properties->lengthInMilliseconds());
            fmt::print(
                "  {:<{}} : {}\n",
                "length",
                WIDTH,
                Utils::formatTime(
                    std::chrono::duration_cast<std::chrono::seconds>(length)));
        }
    }

    // Handle additional file properties if needed
    const TagLib::PropertyMap &fileProperties = f.file()->properties();
    for (const auto &[key, values] : fileProperties) {
        if (!key.isEmpty() && !values.isEmpty()) {
            std::string keyStr = key.stripWhiteSpace().to8Bit(true);
            Utils::toLowercase(keyStr);
            fmt::print("  {:<{}} : ", keyStr, WIDTH);
            for (const auto &value : values) {
                if (!value.isEmpty()) {
                    fmt::print("{}", value.stripWhiteSpace().to8Bit(true));
                }
            }
            fmt::print("\n");
        }
    }

    std::cout.flush();
}

void Decoder::decodeOpus() {
    int error;
    constexpr double sampleRate = 48000;

    std::unique_ptr<OggOpusFile, decltype(&op_free)> of(
        op_open_file(filePath.string().data(), &error), &op_free);
    if (!of) {
        throw std::runtime_error(
            std::format("Error opening Opus file: {}", filePath.string()));
    }

    opus_int64 totalSamples = op_pcm_total(of.get(), -1);
    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalSamples / sampleRate));

    std::ofstream pipe(pipeName, std::ios::binary);
    if (!pipe) {
        throw std::runtime_error(
            std::format("Error opening pipe: {}", pipeName));
    }

    constexpr int channels = 2;
    constexpr int bitsPerSample = 16;
    constexpr size_t bufferSize = 4096;

    std::vector<opus_int16> pcmBuffer(bufferSize * channels);
    int samplesRead;
    std::string durStr = Utils::formatTime(totalDuration);

    while ((samplesRead =
                op_read_stereo(of.get(), pcmBuffer.data(), bufferSize)) > 0) {
        pipe.write(reinterpret_cast<const char *>(pcmBuffer.data()),
                   samplesRead * channels * (bitsPerSample / 8));
        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(op_pcm_tell(of.get()) / sampleRate));
        printDecodingProgress(currentPosition, durStr);
    }

    if (samplesRead < 0) {
        fmt::print(stdout,
                   "\n  {:<{}} : {}",
                   "error",
                   WIDTH,
                   "error decoding Opus file");
    }
}

SoxrHandle::SoxrHandle(double inputRate,
                       double outputRate,
                       int channels,
                       const soxr_datatype_t &in_type,
                       const soxr_datatype_t &out_type,
                       const int quality) {
    iospec = soxr_io_spec(in_type, out_type);
    q_spec = soxr_quality_spec(quality, 0);
    handle = soxr_create(
        inputRate, outputRate, channels, &error, &iospec, &q_spec, nullptr);
    if (!handle) {
        throw std::runtime_error(fmt::format("Failed to create SoXR handle: {}",
                                             soxr_strerror(error)));
    }
}

SoxrHandle::~SoxrHandle() {
    if (handle) {
        soxr_delete(handle);
    }
}

void SoxrHandle::process(const std::vector<int16_t> &audioBuffer,
                         std::vector<int16_t> &resampledBuffer,
                         size_t bytesRead,
                         size_t outBufferSize,
                         size_t *resampledSize) {
    size_t inputLength = bytesRead / 4;
    error =
        soxr_process(handle,
                     reinterpret_cast<const soxr_in_t *>(audioBuffer.data()),
                     inputLength,
                     nullptr,
                     reinterpret_cast<soxr_out_t *>(resampledBuffer.data()),
                     outBufferSize,
                     resampledSize);
    if (error) {
        throw std::runtime_error(
            fmt::format("Failed to process sample: {}", soxr_strerror(error)));
    }
}

void Decoder::decodeMp3() {
    mpg123_handle *mh = nullptr;
    int err;

    // Initialize mpg123
    if (mpg123_init() != MPG123_OK) {
        throw std::runtime_error("Unable to initialize mpg123 library");
    }

    // Create mpg123 handle
    mh = mpg123_new(nullptr, &err);
    if (!mh) {
        mpg123_exit();
        throw std::runtime_error(fmt::format("Error creating mpg123 handle: {}",
                                             mpg123_strerror(mh)));
    }

    // Use a unique_ptr with a custom deleter to manage mpg123 handle
    auto cleanupMpg123 = [](mpg123_handle *handle) {
        mpg123_close(handle);
        mpg123_delete(handle);
        mpg123_exit();
    };
    std::unique_ptr<mpg123_handle, decltype(cleanupMpg123)> mhPtr(
        mh, cleanupMpg123);

    // Open MP3 file
    if (mpg123_open(mhPtr.get(), filePath.string().data()) != MPG123_OK) {
        throw std::runtime_error(
            fmt::format("Error opening MP3 file: {}", filePath.string()));
    }

    long sampleRate;
    int channels, encoding;
    if (mpg123_getformat(mhPtr.get(), &sampleRate, &channels, &encoding) !=
        MPG123_OK) {
        throw std::runtime_error(fmt::format("Error getting MP3 format: {}",
                                             mpg123_strerror(mhPtr.get())));
    }

    constexpr int targetChannels = 2;
    constexpr double targetSampleRate = 48000;
    encoding = MPG123_ENC_SIGNED_16;
    if (mpg123_format_none(mhPtr.get()) != MPG123_OK ||
        mpg123_format(mhPtr.get(),
                      static_cast<long>(targetSampleRate),
                      targetChannels,
                      encoding) != MPG123_OK) {
        throw std::runtime_error(fmt::format("Error setting MP3 format: {}",
                                             mpg123_strerror(mhPtr.get())));
    }

    // Configure the resampler
    SoxrHandle soxrHandle(static_cast<double>(sampleRate),
                          targetSampleRate,
                          targetChannels,
                          SOXR_INT16_I,
                          SOXR_INT16_I,
                          SOXR_HQ);

    std::ofstream pipe(pipeName, std::ios::binary);
    if (!pipe) {
        throw std::runtime_error(
            fmt::format("Error opening pipe: {}", pipeName));
    }

    off_t totalFrames = mpg123_length(mhPtr.get());
    if (totalFrames == MPG123_ERR) {
        throw std::runtime_error(
            std::format("Error getting total length of MP3 file: {}",
                        mpg123_strerror(mhPtr.get())));
    }
    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalFrames / sampleRate));
    std::string durStr = Utils::formatTime(totalDuration);

    constexpr size_t bufferSize = 4096;
    std::vector<int16_t> audioBuffer(bufferSize * channels);
    size_t outBufferSize;
    std::vector<int16_t> resampledBuffer;
    size_t bytesRead;

    fmt::print("  {:<{}} : {} Hz\n", "i-samplerate", WIDTH, sampleRate);
    fmt::print("  {:<{}} : {} Hz\n", "o-samplerate", WIDTH, targetSampleRate);
    if (static_cast<long>(targetSampleRate) != sampleRate) {
        fmt::print("  {:<{}} : {} to {} Hz\n",
                   "resample",
                   WIDTH,
                   sampleRate,
                   static_cast<long>(targetSampleRate));
        double freqRatio = targetSampleRate / sampleRate;
        outBufferSize = static_cast<size_t>(bufferSize * freqRatio + 1.0);
        resampledBuffer.reserve(outBufferSize * targetChannels);
    }

    while ((err = mpg123_read(mhPtr.get(),
                              audioBuffer.data(),
                              bufferSize * channels * sizeof(int16_t),
                              &bytesRead)) == MPG123_OK) {
        if (static_cast<long>(targetSampleRate) != sampleRate) {
            size_t resampledSize;
            soxrHandle.process(audioBuffer,
                               resampledBuffer,
                               bytesRead,
                               outBufferSize,
                               &resampledSize);
            pipe.write(reinterpret_cast<const char *>(resampledBuffer.data()),
                       resampledSize * sizeof(int16_t) * targetChannels);
        } else {
            pipe.write(reinterpret_cast<const char *>(audioBuffer.data()),
                       bytesRead);
        }
        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(mpg123_tell(mhPtr.get()) /
                                          targetSampleRate));
        printDecodingProgress(currentPosition, durStr);
    }

    if (err != MPG123_DONE) {
        fmt::print(stdout,
                   "\n  {:<{}} : {}: {}",
                   "error",
                   WIDTH,
                   "error decoding MP3 file",
                   mpg123_strerror(mhPtr.get()));
    }
}

void Decoder::printDecodingProgress(const std::chrono::seconds currentPosition,
                                    const std::string &durStr) {
    fmt::print("  {:<{}} : {} / {}\r",
               "position",
               WIDTH,
               Utils::formatTime(currentPosition),
               durStr);
    std::cout.flush();
}