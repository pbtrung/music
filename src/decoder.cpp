#include "decoder.hpp"
#include "const.hpp"
#include "fmtlog-inl.hpp"
#include "utils.hpp"
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

namespace fs = std::filesystem;

Decoder::Decoder(const std::filesystem::path &filePath,
                 const std::string_view &extension,
                 const std::string_view &pipeName)
    : filePath(filePath), extension(extension), pipeName(pipeName) {}

void Decoder::decode() {
    logd("start decode");
    if (extension == "mp3") {
        decodeMp3();
    } else if (extension == "opus") {
        decodeOpus();
    } else {
        loge("Unsupported format: {}", filePath.string());
        fmt::print("  {:<{}}: {}: {}", "error", WIDTH, "Unsupported format",
                   filePath.string());
    }
    fmt::print("\n\n");
    logd("finish decode");
}

void Decoder::printMetadata() {
    TagLib::FileRef f(filePath.string().data());
    if (f.isNull()) {
        loge("Invalid or unsupported file: {}", filePath.string());
        fmt::print(stdout, "  {:<{}}: {}: {}\n", "error", WIDTH,
                   "Invalid or unsupported file", filePath.string());
        return;
    }

    auto printTag = [](std::string_view name, const TagLib::String &value) {
        if (!value.isEmpty()) {
            fmt::print("  {:<{}}: {}\n", name, WIDTH,
                       value.stripWhiteSpace().to8Bit(true));
        }
    };

    if (auto *tag = f.tag()) {
        printTag("title", tag->title());
        printTag("artist", tag->artist());
        printTag("album", tag->album());
        if (tag->year() != 0) {
            fmt::print("  {:<{}}: {}\n", "year", WIDTH, tag->year());
        }
        printTag("comment", tag->comment());
        if (tag->track() != 0) {
            fmt::print("  {:<{}}: {}\n", "track", WIDTH, tag->track());
        }
        printTag("genre", tag->genre());
    }

    if (auto *properties = f.audioProperties()) {
        if (properties->bitrate() != 0) {
            fmt::print("  {:<{}}: {} kbps\n", "bitrate", WIDTH,
                       properties->bitrate());
        }
        if (properties->sampleRate() != 0) {
            fmt::print("  {:<{}}: {}\n", "sample-rate", WIDTH,
                       properties->sampleRate());
        }
        if (properties->channels() != 0) {
            fmt::print("  {:<{}}: {}\n", "channels", WIDTH,
                       properties->channels());
        }
        if (properties->lengthInMilliseconds() != 0) {
            auto length =
                std::chrono::milliseconds(properties->lengthInMilliseconds());
            fmt::print(
                "  {:<{}}: {}\n", "length", WIDTH,
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
            fmt::print("  {:<{}}: ", keyStr, WIDTH);
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
    logd("start decode opus");
    constexpr double opusSampleRate = 48000;

    auto of = openOpusFile();

    opus_int64 totalSamples = op_pcm_total(of.get(), -1);
    auto totalDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalSamples / opusSampleRate));

    std::ofstream pipe = openPipe();
    if (!pipe) {
        loge("Failed to open pipe: {}", pipeName);
        throw std::runtime_error(
            fmt::format("Failed to open pipe: {}", pipeName));
    }

    std::string durStr = Utils::formatTime(totalDuration);
    fmtlog::poll(true);
    readAndWriteOpusData(of.get(), pipe, totalSamples, opusSampleRate, durStr);
    logd("finish decode opus");
}

std::unique_ptr<OggOpusFile, decltype(&op_free)> Decoder::openOpusFile() {
    int error;
    OggOpusFile *of = op_open_file(filePath.string().data(), &error);
    if (!of) {
        loge("Failed to open Opus file: {}", filePath.string());
        throw std::runtime_error(
            fmt::format("Failed to open Opus file: {}", filePath.string()));
    }
    return {of, op_free};
}

void Decoder::readAndWriteOpusData(OggOpusFile *of, std::ofstream &pipe,
                                   opus_int64 totalSamples,
                                   const double opusSampleRate,
                                   const std::string &durStr) {
    constexpr int inChannels = 2;
    constexpr size_t bufferSize = 4096;

    std::vector<opus_int16> pcmBuffer(bufferSize * inChannels);
    int samplesRead;

    logd("start decode opus loop");
    fmtlog::poll(true);
    while ((samplesRead = op_read_stereo(of, pcmBuffer.data(), bufferSize)) >
           0) {
        pipe.write(reinterpret_cast<const char *>(pcmBuffer.data()),
                   samplesRead * inChannels * sizeof(opus_int16));
        if (pipe.fail()) {
            loge("Failed to write to pipe");
            throw std::runtime_error("Failed to write to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(op_pcm_tell(of) / opusSampleRate));
        printDecodingProgress(currentPosition, durStr);
    }
    logd("finish decode opus loop");

    if (samplesRead < 0) {
        fmt::print(stdout, "\n  {:<{}}: {}", "error", WIDTH,
                   "failed to decode Opus file");
    }
    fmtlog::poll(true);
}

SoxrResampler::SoxrResampler(double inputRate, double outputRate,
                             int outChannels, soxr_datatype_t in_type,
                             soxr_datatype_t out_type, int quality) {
    soxr_io_spec_t iospec = soxr_io_spec(in_type, out_type);
    soxr_quality_spec_t q_spec = soxr_quality_spec(quality, 0);
    soxr_error_t error;
    handle = soxr_create(inputRate, outputRate, outChannels, &error, &iospec,
                         &q_spec, nullptr);
    if (!handle) {
        loge("Failed to create SoXR handle: {}", soxr_strerror(error));
        throw std::runtime_error(fmt::format("Failed to create SoXR handle: {}",
                                             soxr_strerror(error)));
    }
}

SoxrResampler::~SoxrResampler() {
    soxr_delete(handle);
}

void SoxrResampler::process(const std::vector<int16_t> &audioBuffer,
                            const size_t inputLength,
                            std::vector<int16_t> &resampledBuffer,
                            const size_t outBufferSize, size_t *resampledSize) {
    soxr_error_t error = soxr_process(
        handle, reinterpret_cast<const soxr_in_t *>(audioBuffer.data()),
        inputLength, nullptr,
        reinterpret_cast<soxr_out_t *>(resampledBuffer.data()), outBufferSize,
        resampledSize);

    if (error) {
        loge("Failed to process sample: {}", soxr_strerror(error));
        throw std::runtime_error(
            fmt::format("Failed to process sample: {}", soxr_strerror(error)));
    }
}

void Decoder::decodeMp3() {
    logd("start decode mp3");
    initializeMpg123();

    auto mhPtr = createMpg123Handle();

    openMp3File(mhPtr.get());

    long inSampleRate;
    int inChannels, encoding;
    getMp3Format(mhPtr.get(), &inSampleRate, &inChannels, &encoding);

    constexpr int outChannels = 2;
    constexpr long outSampleRate = 48000;
    encoding = MPG123_ENC_SIGNED_16;
    setMp3Format(mhPtr.get(), outChannels, outSampleRate, encoding);

    SoxrResampler soxrResampler(inSampleRate, outSampleRate, outChannels,
                                SOXR_INT16_I, SOXR_INT16_I, SOXR_HQ);

    std::ofstream pipe = openPipe();
    if (!pipe) {
        loge("Failed to open pipe: {}", pipeName);
        throw std::runtime_error(
            fmt::format("Failed to open pipe: {}", pipeName));
    }

    std::chrono::seconds totalDuration =
        getMp3TotalDuration(mhPtr.get(), inSampleRate);
    std::string durStr = Utils::formatTime(totalDuration);

    fmtlog::poll(true);
    readResampleAndWriteMp3Data(mhPtr.get(), pipe, inSampleRate, inChannels,
                                totalDuration, soxrResampler);
    logd("finish decode mp3");
}

std::unique_ptr<mpg123_handle, std::function<void(mpg123_handle *)>>
Decoder::createMpg123Handle() {
    int err;
    mpg123_handle *mh = mpg123_new(nullptr, &err);
    if (!mh) {
        loge("Failed to create mpg123 handle: {}", mpg123_strerror(mh));
        throw std::runtime_error(fmt::format(
            "Failed to create mpg123 handle: {}", mpg123_strerror(mh)));
    }
    auto cleanupMpg123 = [](mpg123_handle *handle) {
        if (handle) {
            mpg123_close(handle);
            mpg123_delete(handle);
            mpg123_exit();
        }
    };
    return {mh, cleanupMpg123};
}

void Decoder::initializeMpg123() {
    if (mpg123_init() != MPG123_OK) {
        loge("Failed to initialize mpg123 library");
        throw std::runtime_error("Failed to initialize mpg123 library");
    }
}

void Decoder::openMp3File(mpg123_handle *mh) {
    if (mpg123_open(mh, filePath.string().data()) != MPG123_OK) {
        loge("Failed to open MP3 file: {}", filePath.string());
        throw std::runtime_error(
            fmt::format("Failed to open MP3 file: {}", filePath.string()));
    }
}

void Decoder::getMp3Format(mpg123_handle *mh, long *inSampleRate,
                           int *inChannels, int *encoding) {
    if (mpg123_getformat(mh, inSampleRate, inChannels, encoding) != MPG123_OK) {
        loge("Failed to get MP3 format: {}", mpg123_strerror(mh));
        throw std::runtime_error(
            fmt::format("Failed to get MP3 format: {}", mpg123_strerror(mh)));
    }
}

void Decoder::setMp3Format(mpg123_handle *mh, int outChannels,
                           long outSampleRate, int encoding) {
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, outSampleRate, outChannels, encoding) != MPG123_OK) {
        loge("Failed to set MP3 format: {}", mpg123_strerror(mh));
        throw std::runtime_error(
            fmt::format("Failed to set MP3 format: {}", mpg123_strerror(mh)));
    }
}

std::chrono::seconds Decoder::getMp3TotalDuration(mpg123_handle *mh,
                                                  long inSampleRate) {
    off_t totalFrames = mpg123_length(mh);
    if (totalFrames == MPG123_ERR) {
        loge("Failed to get total length of MP3 file: {}", mpg123_strerror(mh));
        throw std::runtime_error(fmt::format(
            "Failed to get total length of MP3 file: {}", mpg123_strerror(mh)));
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::duration<double>(totalFrames / inSampleRate));
}

void Decoder::readResampleAndWriteMp3Data(
    mpg123_handle *mh, std::ofstream &pipe, long inSampleRate, int inChannels,
    const std::chrono::seconds totalDuration, SoxrResampler &soxrResampler) {

    constexpr size_t bufferSize = 4096;
    std::vector<int16_t> audioBuffer(bufferSize * inChannels);
    size_t outBufferSize;
    std::vector<int16_t> resampledBuffer;
    size_t bytesRead;

    constexpr int outChannels = 2;
    constexpr long outSampleRate = 48000;

    fmt::print("  {:<{}}: {}\n", "i-samplerate", WIDTH, inSampleRate);
    fmt::print("  {:<{}}: {}\n", "o-samplerate", WIDTH, outSampleRate);
    if (outSampleRate != inSampleRate) {
        fmt::print("  {:<{}}: {} -> {}\n", "resample", WIDTH, inSampleRate,
                   outSampleRate);
        double freqRatio = outSampleRate / static_cast<double>(inSampleRate);
        outBufferSize = static_cast<size_t>(bufferSize * freqRatio + 1.0);
        resampledBuffer.reserve(outBufferSize * outChannels);
    }

    int err;
    std::string durStr = Utils::formatTime(totalDuration);

    logd("start decode mp3 loop");
    fmtlog::poll(true);
    while ((err = mpg123_read(mh, audioBuffer.data(),
                              bufferSize * inChannels * sizeof(int16_t),
                              &bytesRead)) == MPG123_OK) {
        if (outSampleRate != inSampleRate) {
            size_t resampledSize;
            size_t inputLength = (bytesRead / sizeof(int16_t)) / inChannels;
            soxrResampler.process(audioBuffer, inputLength, resampledBuffer,
                                  outBufferSize, &resampledSize);
            pipe.write(reinterpret_cast<const char *>(resampledBuffer.data()),
                       resampledSize * outChannels * sizeof(int16_t));
        } else {
            pipe.write(reinterpret_cast<const char *>(audioBuffer.data()),
                       bytesRead);
        }
        if (pipe.fail()) {
            loge("Failed to write to pipe");
            throw std::runtime_error("Failed to write to pipe");
        }

        auto currentPosition = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::duration<double>(mpg123_tell(mh) / inSampleRate));
        printDecodingProgress(currentPosition, durStr);
    }
    logd("finish decode mp3 loop");

    if (err != MPG123_DONE) {
        loge("Failed to decode MP3 file: {}", mpg123_strerror(mh));
        fmt::print(stdout, "\n  {:<{}}: {}: {}", "error", WIDTH,
                   "failed to decode MP3 file", mpg123_strerror(mh));
    }
    fmtlog::poll(true);
}

std::ofstream Decoder::openPipe() {
    return std::ofstream(pipeName, std::ios::binary);
}

void Decoder::printDecodingProgress(const std::chrono::seconds currentPosition,
                                    const std::string &durStr) {
    fmt::print("  {:<{}}: {} / {}\r", "position", WIDTH,
               Utils::formatTime(currentPosition), durStr);
    std::cout.flush();
}