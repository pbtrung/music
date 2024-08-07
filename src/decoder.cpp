#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <sndfile.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

namespace fs = std::filesystem;

Decoder::Decoder(const std::filesystem::path &filePath,
                 std::string_view extension,
                 std::string_view pipeName)
    : filePath(filePath), extension(extension), pipeName(pipeName) {}

void Decoder::decode() {
    if (extension == "opus" || extension == "mp3" || extension == "ogg" ||
        extension == "flac" || extension == "m4a" || extension == "wav") {
        decodeSndFile();
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

void Decoder::decodeSndFile() {
    SF_INFO sfInfo;
    SNDFILE *sndFileRaw = sf_open(filePath.string().data(), SFM_READ, &sfInfo);
    if (!sndFileRaw) {
        throw std::runtime_error(
            std::format("Error opening sound file: {}", filePath.string()));
    }

    auto sndFile =
        std::unique_ptr<SNDFILE, decltype(&sf_close)>(sndFileRaw, &sf_close);

    // clang-format off
    bool isSupportedFormat = (sfInfo.format == SF_FORMAT_WAV ||
                              sfInfo.format == SF_FORMAT_FLAC ||
                              sfInfo.format == SF_FORMAT_OGG ||
                              sfInfo.format == SF_FORMAT_VORBIS ||
                              sfInfo.format == SF_FORMAT_OPUS ||
                              sfInfo.format == SF_FORMAT_ALAC_16 ||
                              sfInfo.format == SF_FORMAT_ALAC_20 ||
                              sfInfo.format == SF_FORMAT_ALAC_24 ||
                              sfInfo.format == SF_FORMAT_ALAC_32 ||
                              sfInfo.format == SF_FORMAT_MPEG_LAYER_III);
    // clang-format on

    if (!isSupportedFormat) {
        throw std::runtime_error(fmt::format(
            "  {:<{}} : {}\n", "error", WIDTH, "Unsupported format"));
    }

    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(sfInfo.frames /
                                      static_cast<double>(sfInfo.samplerate)));

    std::ofstream pipe(pipeName, std::ios::binary);
    if (!pipe) {
        throw std::runtime_error(
            std::format("Error opening pipe: {}", pipeName));
    }

    constexpr size_t bufferSize = 4096;
    std::vector<short> buffer(bufferSize);
    sf_count_t framesRead;
    std::string durStr = Utils::formatTime(totalDuration);

    while ((framesRead =
                sf_readf_short(sndFile.get(), buffer.data(), bufferSize)) > 0) {
        pipe.write(reinterpret_cast<const char *>(buffer.data()),
                   framesRead * sfInfo.channels * sizeof(short));
        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(
                sf_seek(sndFile.get(), 0, SEEK_CUR) /
                static_cast<double>(sfInfo.samplerate)));
        fmt::print("  {:<{}} : {} / {}\r",
                   "position",
                   WIDTH,
                   Utils::formatTime(currentPosition),
                   durStr);
        std::cout.flush();
    }

    // Check for any errors during reading
    if (sf_error(sndFile.get()) != SF_ERR_NO_ERROR) {
        fmt::print(stdout,
                   "\n  {:<{}} : {}",
                   "error",
                   WIDTH,
                   "error decoding sound file");
    }
}