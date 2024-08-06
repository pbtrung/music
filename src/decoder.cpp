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

decoder::decoder(const std::filesystem::path &file_path, const std::string &ext,
                 const std::string &pipe_name)
    : file_path(file_path), ext(ext), pipe_name(pipe_name) {}

static void decode_opus(const std::string &filename,
                        const std::string &pipe_name) {
    int error;
    const int channels = 2;
    const int bits_per_sample = 16;
    const size_t buffer_size = 4096;
    const double sample_rate = 48000.0;

    std::unique_ptr<OggOpusFile, decltype(&op_free)> of(
        op_open_file(filename.c_str(), &error), &op_free);
    if (!of) {
        throw std::runtime_error(
            fmt::format("Error opening file: {}", filename));
    }

    opus_int64 total_samples = op_pcm_total(of.get(), -1);
    double total_seconds = static_cast<double>(total_samples) / sample_rate;
    std::string total_time = utils::get_time(total_seconds);

    std::ofstream pipe(pipe_name, std::ios::binary);
    if (!pipe.is_open()) {
        throw std::runtime_error(
            fmt::format("Error opening pipe: {}", pipe_name));
    }

    std::vector<opus_int16> pcm(buffer_size * channels);
    int ret;
    while ((ret = op_read_stereo(of.get(), pcm.data(), buffer_size)) > 0) {
        pipe.write(reinterpret_cast<char *>(pcm.data()),
                   ret * channels * (bits_per_sample / 8));
        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        opus_int64 position = op_pcm_tell(of.get());
        double seconds = static_cast<double>(position) / sample_rate;
        std::string time = utils::get_time(seconds);
        fmt::print("  {:<{}} : {} / {}\r", "position", WIDTH, time, total_time);
        std::cout.flush();
    }

    if (ret < 0) {
        throw std::runtime_error("Error decoding Opus file");
    }
}

static void decode_mp3(const std::string &filename,
                       const std::string &pipe_name) {
    mpg123_handle *mh = nullptr;
    int err;
    std::vector<unsigned char> audio(4096);
    size_t done;

    if (mpg123_init() != MPG123_OK) {
        throw std::runtime_error("Unable to initialize mpg123 library");
    }

    mh = mpg123_new(nullptr, &err);
    if (!mh) {
        throw std::runtime_error("Error creating mpg123 handle");
    }

    auto cleanup = [](mpg123_handle *mh) {
        mpg123_close(mh);
        mpg123_delete(mh);
        mpg123_exit();
    };
    std::unique_ptr<mpg123_handle, decltype(cleanup)> mh_ptr(mh, cleanup);

    if (mpg123_open(mh, filename.c_str()) != MPG123_OK) {
        throw std::runtime_error(
            fmt::format("Error opening file: {}", filename));
    }

    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) {
        throw std::runtime_error("Error getting MP3 format");
    }

    channels = 2;
    encoding = MPG123_ENC_SIGNED_16;
    rate = 48000;

    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, rate, channels, encoding) != MPG123_OK) {
        throw std::runtime_error("Error setting format");
    }

    off_t total_frames = mpg123_length(mh);
    if (total_frames == -1) {
        throw std::runtime_error("Error getting total length of MP3 file");
    }
    double total_seconds = total_frames / static_cast<double>(rate);
    std::string total_time = utils::get_time(total_seconds);

    std::ofstream pipe(pipe_name, std::ios::binary);
    if (!pipe.is_open()) {
        throw std::runtime_error(
            fmt::format("Error opening pipe: {}", pipe_name));
    }

    while ((err = mpg123_read(mh, audio.data(), audio.size(), &done)) ==
           MPG123_OK) {
        pipe.write(reinterpret_cast<char *>(audio.data()), done);
        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        off_t position = mpg123_tell(mh);
        double seconds = static_cast<double>(position) / rate;
        std::string time = utils::get_time(seconds);
        fmt::print("  {:<{}} : {} / {}\r", "position", WIDTH, time, total_time);
        std::cout.flush();
    }

    if (err != MPG123_DONE) {
        throw std::runtime_error("Error decoding MP3 file");
    }
}

void decoder::decode() {
    if (ext == "mp3") {
        decode_mp3(file_path.string(), pipe_name);
    } else if (ext == "opus") {
        decode_opus(file_path.string(), pipe_name);
    } else {
        fmt::print("  {:<{}} : {}", "error", WIDTH, "Unsupported format");
    }
    fmt::print("\n\n");
}

void decoder::print_metadata() {
    TagLib::FileRef file(file_path.string().c_str());
    if (!file.isNull() && file.tag()) {
        TagLib::Tag *tag = file.tag();

        auto print_tag = [](const std::string &name, const auto &value) {
            if (!value.isEmpty()) {
                fmt::print("  {:<{}} : {}\n", name, WIDTH,
                           value.stripWhiteSpace().to8Bit(true));
            }
        };

        print_tag("title", tag->title());
        print_tag("artist", tag->artist());
        print_tag("album", tag->album());
        if (tag->year() != 0) {
            fmt::print("  {:<{}} : {}\n", "year", WIDTH, tag->year());
        }
        print_tag("comment", tag->comment());
        if (tag->track() != 0) {
            fmt::print("  {:<{}} : {}\n", "track", WIDTH, tag->track());
        }
        print_tag("genre", tag->genre());
    }

    if (!file.isNull() && file.audioProperties()) {
        TagLib::AudioProperties *properties = file.audioProperties();

        if (properties->bitrate() != 0) {
            fmt::print("  {:<{}} : {}\n", "bitrate", WIDTH,
                       properties->bitrate());
        }
        if (properties->sampleRate() != 0) {
            fmt::print("  {:<{}} : {}\n", "sample-rate", WIDTH,
                       properties->sampleRate());
        }
        if (properties->channels() != 0) {
            fmt::print("  {:<{}} : {}\n", "channels", WIDTH,
                       properties->channels());
        }
        if (properties->lengthInMilliseconds() != 0) {
            double seconds =
                static_cast<double>(properties->lengthInMilliseconds()) / 1000;
            std::string time = utils::get_time(seconds);
            fmt::print("  {:<{}} : {}\n", "length", WIDTH, time);
        }
    }

    if (!file.isNull()) {
        const TagLib::PropertyMap properties = file.file()->properties();
        for (const auto &[key, values] : properties) {
            if (!key.isEmpty() && !values.isEmpty()) {
                std::string key_str = key.stripWhiteSpace().to8Bit(true);
                utils::to_lowercase(key_str);
                fmt::print("  {:<{}} : ", key_str, WIDTH);
                for (const auto &value : values) {
                    if (!value.isEmpty()) {
                        fmt::print("{}", value.stripWhiteSpace().to8Bit(true));
                    }
                }
                fmt::print("\n");
            }
        }
    }
}