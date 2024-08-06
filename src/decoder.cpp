#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <mpg123.h>
#include <opusfile.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

namespace fs = std::filesystem;

Decoder::Decoder(const std::filesystem::path &filePath,
                 std::string_view extension, std::string_view pipeName)
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
        fmt::print("  {:<{}} : {}\n", "error", WIDTH,
                   "Invalid or unsupported file");
        return;
    }

    auto printTag = [](std::string_view name, const TagLib::String &value) {
        if (!value.isEmpty()) {
            fmt::print("  {:<{}} : {}\n", name, WIDTH, value.to8Bit(true));
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
            fmt::print("  {:<{}} : {} kbps\n", "bitrate", WIDTH,
                       properties->bitrate());
        }
        if (properties->sampleRate() != 0) {
            fmt::print("  {:<{}} : {} Hz\n", "sample-rate", WIDTH,
                       properties->sampleRate());
        }
        if (properties->channels() != 0) {
            fmt::print("  {:<{}} : {}\n", "channels", WIDTH,
                       properties->channels());
        }
        if (properties->lengthInMilliseconds() != 0) {
            auto length =
                std::chrono::milliseconds(properties->lengthInMilliseconds());
            fmt::print(
                "  {:<{}} : {}\n", "length", WIDTH,
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
        pipe.write(reinterpret_cast<char *>(pcmBuffer.data()),
                   samplesRead * channels * (bitsPerSample / 8));

        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(op_pcm_tell(of.get()) / sampleRate));
        fmt::print("  {:<{}} : {} / {}\r", "position", WIDTH,
                   Utils::formatTime(currentPosition), durStr);
        std::cout.flush();
    }

    if (samplesRead < 0) {
        fmt::print(stdout, "\n  {:<{}} : {}", "error", WIDTH,
                   "error decoding Opus file");
    }
}

void Decoder::decodeMp3() {
    mpg123_handle *mh = nullptr;
    int err;

    if (mpg123_init() != MPG123_OK) {
        throw std::runtime_error("Unable to initialize mpg123 library");
    }

    mh = mpg123_new(nullptr, &err);
    if (!mh) {
        mpg123_exit();
        throw std::runtime_error(std::format("Error creating mpg123 handle: {}",
                                             mpg123_strerror(mh)));
    }

    auto cleanup = [](mpg123_handle *mh) {
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
    };
    std::unique_ptr<mpg123_handle, decltype(cleanup)> mhPtr(mh, cleanup);

    if (mpg123_open(mh, filePath.string().data()) != MPG123_OK) {
        throw std::runtime_error(
            std::format("Error opening MP3 file: {}", filePath.string()));
    }

    long sampleRate;
    int channels, encoding;
    if (mpg123_getformat(mh, &sampleRate, &channels, &encoding) != MPG123_OK) {
        throw std::runtime_error(
            std::format("Error getting MP3 format: {}", mpg123_strerror(mh)));
    }

    sampleRate = 48000;
    channels = 2;
    encoding = MPG123_ENC_SIGNED_16;
    // Force stereo 16-bit 48kHz output
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, sampleRate, channels, encoding) != MPG123_OK) {
        throw std::runtime_error(
            std::format("Error setting MP3 format: {}", mpg123_strerror(mh)));
    }

    off_t totalFrames = mpg123_length(mh);
    if (totalFrames == MPG123_ERR) { // Check for error specifically
        throw std::runtime_error(std::format(
            "Error getting total length of MP3 file: {}", mpg123_strerror(mh)));
    }

    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalFrames /
                                      static_cast<double>(sampleRate)));

    std::ofstream pipe(pipeName, std::ios::binary);
    if (!pipe) {
        throw std::runtime_error(
            std::format("Error opening pipe: {}", pipeName));
    }

    constexpr size_t bufferSize = 4096;
    std::vector<unsigned char> audioBuffer(bufferSize);
    size_t bytesRead;
    std::string durStr = Utils::formatTime(totalDuration);

    while ((err = mpg123_read(mh, audioBuffer.data(), bufferSize,
                              &bytesRead)) == MPG123_OK) {
        pipe.write(reinterpret_cast<char *>(audioBuffer.data()), bytesRead);

        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(mpg123_tell(mh) /
                                          static_cast<double>(sampleRate)));
        fmt::print("  {:<{}} : {} / {}\r", "position", WIDTH,
                   Utils::formatTime(currentPosition), durStr);
        std::cout.flush();
    }

    // Check for decoding errors explicitly and print detailed error
    if (err != MPG123_DONE) {
        fmt::print(stdout, "\n  {:<{}} : {}: {}", "error", WIDTH,
                   "error decoding MP3 file", mpg123_strerror(mh));
    }
}