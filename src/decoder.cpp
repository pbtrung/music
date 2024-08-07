#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <sndfile.hh>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

namespace fs = std::filesystem;

Decoder::Decoder(const std::filesystem::path &filePath,
                 std::string_view pipeName)
    : filePath(filePath), pipeName(pipeName) {}

void Decoder::decode() {
    decodeSndFile();
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
    SndfileHandle infile(filePath.string());
    if (infile.error()) {
        throw std::runtime_error(fmt::format("{}: {} File: {}",
                                             "Error opening sound file",
                                             infile.strError(),
                                             filePath.string()));
    }
    SndfileHandle outfile(pipeName,
                          SFM_WRITE,
                          SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE,
                          2,
                          48000);
    if (outfile.error()) {
        throw std::runtime_error(fmt::format("{}: {} File: {}",
                                             "Error opening pipe",
                                             outfile.strError(),
                                             pipeName));
    }

    constexpr size_t bufferSize = 4096;
    std::vector<short> buffer(bufferSize * infile.channels());

    // Calculate and display duration
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(static_cast<double>(infile.frames()) /
                                      infile.samplerate()));
    std::string durationStr = Utils::formatTime(duration);

    sf_count_t framesRead;
    while ((framesRead = infile.readf(buffer.data(), bufferSize)) > 0) {
        sf_count_t framesWritten = outfile.writef(buffer.data(), framesRead);
        if (framesWritten != framesRead) {
            throw std::runtime_error("Error writing to pipe");
        }

        // Calculate and display progress
        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(
                static_cast<double>(infile.seek(0, SEEK_CUR)) /
                infile.samplerate()));
        fmt::print("  {:<{}} : {} / {}\r",
                   "position",
                   WIDTH,
                   Utils::formatTime(currentPosition),
                   durationStr);
        std::cout.flush();
    }
}