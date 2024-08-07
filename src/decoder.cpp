#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <samplerate.h>
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
    // Open the input file
    SndfileHandle sndFile(filePath.string());
    if (sndFile.error()) {
        throw std::runtime_error(fmt::format("{}: {} File: {}",
                                             "Error opening sound file",
                                             sndFile.strError(),
                                             filePath.string()));
    }

    // Initialize resampler
    constexpr int targetSampleRate = 48000;
    constexpr int targetChannels = 2;
    constexpr size_t bufferSize = 4096;

    // Initialize resampler state
    SRC_STATE *srcState =
        src_new(SRC_SINC_BEST_QUALITY, sndFile.channels(), nullptr);
    if (!srcState) {
        throw std::runtime_error("Error initializing resampler");
    }
    auto srcStateDeleter = [](SRC_STATE *state) { src_delete(state); };
    std::unique_ptr<SRC_STATE, decltype(srcStateDeleter)> srcStatePtr(
        srcState, srcStateDeleter);

    // Set the resampling ratio
    if (src_set_ratio(srcState,
                      static_cast<double>(targetSampleRate) /
                          sndFile.samplerate()) != 0) {
        throw std::runtime_error("Error setting resampler ratio");
    }

    // Open the output pipe
    SndfileHandle pipe(pipeName,
                       SFM_WRITE,
                       SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE,
                       targetChannels,
                       targetSampleRate);
    if (pipe.error()) {
        throw std::runtime_error(fmt::format("{}: {} File: {}",
                                             "Error opening pipe",
                                             pipe.strError(),
                                             pipeName));
    }

    std::vector<float> inputBuffer(bufferSize * sndFile.channels());
    // std::vector<float> outputBuffer(bufferSize * targetChannels);

    sf_count_t framesRead;
    std::string durStr =
        Utils::formatTime(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(
                static_cast<double>(sndFile.frames()) / sndFile.samplerate())));

    while ((framesRead = sndFile.readf(inputBuffer.data(), bufferSize)) > 0) {
        // SRC_DATA srcData;
        // srcData.data_in = inputBuffer.data();
        // srcData.data_out = outputBuffer.data();
        // srcData.input_frames = framesRead;
        // srcData.output_frames = bufferSize;
        // srcData.end_of_input = 0;
        // srcData.src_ratio =
        //     static_cast<double>(targetSampleRate) / sndFile.samplerate();

        // int result = src_process(srcState, &srcData);
        // if (result != 0) {
        //     throw std::runtime_error(fmt::format("Error during resampling: {}",
        //                                          src_strerror(result)));
        // }

        pipe.writef(outputBuffer.data(), framesRead);
        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(
                static_cast<double>(sndFile.seek(0, SEEK_CUR)) /
                sndFile.samplerate()));
        fmt::print("  {:<{}} : {} / {}\r",
                   "position",
                   WIDTH,
                   Utils::formatTime(currentPosition),
                   durStr);
        std::cout.flush();
    }

    if (sndFile.error() != SF_ERR_NO_ERROR) {
        fmt::print(stdout,
                   "\n  {:<{}} : {}",
                   "error",
                   WIDTH,
                   "error decoding sound file");
    }
}