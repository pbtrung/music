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
        throw std::runtime_error(fmt::format(
            "  {:<{}} : {}", "error", WIDTH, "Invalid or unsupported file"));
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
        throw std::runtime_error(fmt::format("  {:<{}} : {}: {}",
                                             "error",
                                             WIDTH,
                                             "Failed to create SoXR handle",
                                             soxr_strerror(error)));
    }
}

SoxrHandle::~SoxrHandle() {
    if (handle) {
        soxr_delete(handle);
    }
}

soxr_t SoxrHandle::get() const {
    return handle;
}

void SoxrHandle::process(const std::vector<short> &audioBuffer,
                         std::vector<short> &resampledBuffer,
                         size_t framesRead,
                         size_t *resampledSize) {
    error =
        soxr_process(handle,
                     reinterpret_cast<const soxr_in_t *>(audioBuffer.data()),
                     framesRead,
                     nullptr,
                     reinterpret_cast<soxr_out_t *>(resampledBuffer.data()),
                     resampledBuffer.capacity(),
                     resampledSize);
    if (error) {
        throw std::runtime_error(fmt::format("  {:<{}} : {}: {}",
                                             "error",
                                             WIDTH,
                                             "Failed to process sample",
                                             soxr_strerror(error)));
    }
}

void Decoder::decodeSndFile() {
    SndfileHandle infile(filePath.string());
    if (infile.error() != SF_ERR_NO_ERROR) {
        throw std::runtime_error(fmt::format("  {:<{}} : {}: {} File: {}",
                                             "error",
                                             WIDTH,
                                             "Error opening sound file",
                                             infile.strError(),
                                             filePath.string()));
    }
    SndfileHandle outfile(pipeName,
                          SFM_WRITE,
                          SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE,
                          2,
                          48000);
    if (outfile.error() != SF_ERR_NO_ERROR) {
        throw std::runtime_error(fmt::format("  {:<{}} : {}: {} File: {}",
                                             "error",
                                             WIDTH,
                                             "Error opening pipe",
                                             outfile.strError(),
                                             pipeName));
    }

    // Configure the resampler
    SoxrHandle soxrHandle(static_cast<double>(infile.samplerate()),
                          48000,
                          2,
                          SOXR_INT16_I,
                          SOXR_INT16_I,
                          SOXR_HQ);

    // Calculate and display duration
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(static_cast<double>(infile.frames()) /
                                      infile.samplerate()));
    std::string durationStr = Utils::formatTime(duration);

    constexpr size_t bufferSize = 4096;
    std::vector<short> buffer(bufferSize * infile.channels());
    std::vector<short> resampledBuffer;
    sf_count_t framesRead, framesWritten;

    fmt::print(
        "  {:<{}} : {} Hz\n", "infile.samplerate", WIDTH, infile.samplerate());
    fmt::print("  {:<{}} : {} Hz\n",
               "outfile.samplerate",
               WIDTH,
               outfile.samplerate());
    if (static_cast<int>(outfile.samplerate()) != infile.samplerate()) {
        fmt::print("  {:<{}} : {} to {} Hz\n",
                   "resample",
                   WIDTH,
                   infile.samplerate(),
                   outfile.samplerate());
        double freqRatio =
            outfile.samplerate() / static_cast<double>(infile.samplerate());
        resampledBuffer.reserve(
            static_cast<size_t>(bufferSize * freqRatio + 1.0) *
            outfile.channels());
    }

    while ((framesRead = infile.readf(buffer.data(), bufferSize)) > 0) {
        if (static_cast<int>(outfile.samplerate()) == infile.samplerate()) {
            framesWritten = outfile.writef(buffer.data(), framesRead);
        } else {
            size_t resampledSize;
            soxrHandle.process(
                buffer, resampledBuffer, framesRead, &resampledSize);
            framesWritten =
                outfile.writef(resampledBuffer.data(), resampledSize);
            framesRead = resampledSize;
        }
        if (framesWritten != framesRead) {
            throw std::runtime_error(fmt::format(
                "  {:<{}} : {}", "error", WIDTH, "Error writing to pipe"));
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