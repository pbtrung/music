#include "decoder.hpp"
#include "const.hpp"
#include "utils.hpp"
#include <fmt/core.h>
#include <fstream>
#include <mpg123.h>
#include <opusfile.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

decoder::decoder(const std::filesystem::path &file_path, const std::string &ext,
                 const std::string &pipe_name)
    : file_path(file_path), ext(ext), pipe_name(pipe_name) {}

static std::string get_time(double seconds) {
    int hours = static_cast<int>(seconds / 3600);
    seconds -= hours * 3600;
    int minutes = static_cast<int>(seconds / 60);
    seconds -= minutes * 60;
    int milliseconds =
        static_cast<int>((seconds - static_cast<int>(seconds)) * 1000);

    std::string time = "00:00.000";
    if (hours > 0) {
        time = fmt::format("{:02}:{:02}:{:02}.{:03}", hours, minutes,
                           static_cast<int>(seconds), milliseconds);
    } else {
        time = fmt::format("{:02}:{:02}.{:03}", minutes,
                           static_cast<int>(seconds), milliseconds);
    }
    return time;
}

static void decode_opus(const std::string &filename,
                        const std::string &pipe_name) {
    int error;
    int channels = 2;
    int bits_per_sample = 16;
    const size_t buffer_size = 4096;
    double sample_rate = 48000;

    // Open the Opus file with RAII to ensure proper cleanup
    std::unique_ptr<OggOpusFile, decltype(&op_free)> of(
        op_open_file(filename.c_str(), &error), &op_free);
    if (!of) {
        throw std::runtime_error(
            fmt::format("Error opening file: {}", filename));
    }

    opus_int64 total_samples = op_pcm_total(of.get(), -1);
    double total_seconds = static_cast<double>(total_samples) / sample_rate;
    std::string total_time = get_time(total_seconds);

    // Open the named pipe with RAII for resource management
    std::ofstream pipe(pipe_name, std::ios::binary);
    if (!pipe.is_open()) {
        throw std::runtime_error(
            fmt::format("Error opening pipe: {}", pipe_name));
    }

    // PCM buffer using std::vector (no manual memory management)
    std::vector<opus_int16> pcm(buffer_size * channels);

    int ret;
    while ((ret = op_read_stereo(of.get(), pcm.data(), buffer_size)) > 0) {
        pipe.write(reinterpret_cast<char *>(pcm.data()),
                   ret * channels * (bits_per_sample / 8));
        if (pipe.fail()) {
            throw std::runtime_error("Error writing to pipe");
        }

        // Get the current position in samples
        opus_int64 position = op_pcm_tell(of.get());
        double seconds = static_cast<double>(position) / sample_rate;

        std::string time = get_time(seconds);
        fmt::print("\r  {:<{}}: {} / {}", "position", WIDTH, time, total_time);
        std::cout.flush();
    }

    if (ret < 0) {
        throw std::runtime_error("Error decoding Opus file");
    }
    // No need for explicit cleanup in this case,
    // unique_ptr and ofstream handle resource release automatically
}

static void decode_mp3(const std::string &filename,
                       const std::string &pipe_name) {
    mpg123_handle *mh;
    int err;
    std::vector<unsigned char> audio;
    size_t done;
    size_t buffer_size = 4096;

    // Initialize mpg123 library
    if (mpg123_init() != MPG123_OK) {
        throw std::runtime_error("Unable to initialize mpg123 library");
    }

    // Create a new mpg123 handle
    mh = mpg123_new(NULL, &err);
    if (!mh) {
        throw std::runtime_error("Error creating mpg123 handle");
    }

    // Open the MP3 file
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

    // Ensure the output format is correct
    if (mpg123_format_none(mh) != MPG123_OK ||
        mpg123_format(mh, rate, channels, encoding) != MPG123_OK) {
        throw std::runtime_error("Error setting format");
    }

    // Get total length in frames
    off_t total_frames = mpg123_length(mh);
    if (total_frames == -1) {
        throw std::runtime_error("Error getting total length of MP3 file");
    }
    double total_seconds = total_frames / static_cast<double>(rate);
    std::string total_time = get_time(total_seconds);

    // Use std::ofstream to open named pipe
    std::ofstream pipe(pipe_name, std::ios::binary);
    if (!pipe.is_open()) {
        throw std::runtime_error(
            fmt::format("Error opening pipe: {}", pipe_name));
    }

    audio.resize(buffer_size);

    // Read and decode MP3 data
    while ((err = mpg123_read(mh, audio.data(), buffer_size, &done)) ==
           MPG123_OK) {
        pipe.write(reinterpret_cast<char *>(audio.data()), done);

        // Get current position in samples
        off_t position = mpg123_tell(mh);
        double seconds = static_cast<double>(position) / rate;

        std::string time = get_time(seconds);
        fmt::print("\r  {:<{}}: {} / {}", "position", WIDTH, time, total_time);
        std::cout.flush();
    }

    if (err != MPG123_DONE) {
        throw std::runtime_error("Error decoding MP3 file");
    }

    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
}

void decoder::decode() {
    // pipe_name = "test.pcm";
    if (ext == "mp3") {
        decode_mp3(file_path.string(), pipe_name);
    } else if (ext == "opus") {
        decode_opus(file_path.string(), pipe_name);
    }
    fmt::print("\n\n");
}

void decoder::print_metadata() {
    TagLib::FileRef file(file_path.string().data());
    if (!file.isNull() && file.tag()) {
        TagLib::Tag *tag = file.tag();

        if (!tag->title().isEmpty()) {
            fmt::print("  {:<{}}: {}\n", "title", WIDTH,
                       tag->title().toCString(true));
        }
        if (!tag->artist().isEmpty()) {
            fmt::print("  {:<{}}: {}\n", "artist", WIDTH,
                       tag->artist().toCString(true));
        }
        if (!tag->album().isEmpty()) {
            fmt::print("  {:<{}}: {}\n", "album", WIDTH,
                       tag->album().toCString(true));
        }
        if (tag->year() != 0) {
            fmt::print("  {:<{}}: {}\n", "year", WIDTH, tag->year());
        }
        if (!tag->comment().isEmpty()) {
            fmt::print("  {:<{}}: {}\n", "comment", WIDTH,
                       tag->comment().toCString(true));
        }
        if (tag->track() != 0) {
            fmt::print("  {:<{}}: {}\n", "track", WIDTH, tag->track());
        }
        if (!tag->genre().isEmpty()) {
            fmt::print("  {:<{}}: {}\n", "genre", WIDTH,
                       tag->genre().toCString(true));
        }
    }

    if (!file.isNull() && file.audioProperties()) {
        TagLib::AudioProperties *properties = file.audioProperties();

        if (properties->bitrate() != 0) {
            fmt::print("  {:<{}}: {}\n", "bitrate", WIDTH,
                       properties->bitrate());
        }
        if (properties->sampleRate() != 0) {
            fmt::print("  {:<{}}: {}\n", "sample rate", WIDTH,
                       properties->sampleRate());
        }
        if (properties->channels() != 0) {
            fmt::print("  {:<{}}: {}\n", "channels", WIDTH,
                       properties->channels());
        }
        if (properties->length() != 0) {
            fmt::print("  {:<{}}: {} {}\n", "length", WIDTH,
                       properties->length(), std::string("seconds"));
        }
    }

    // Print all additional properties if available
    if (!file.isNull()) {
        const TagLib::PropertyMap properties = file.file()->properties();
        for (TagLib::PropertyMap::ConstIterator it = properties.begin();
             it != properties.end(); ++it) {
            if (!it->first.isEmpty() && !it->second.isEmpty()) {
                std::string key = it->first.to8Bit(true);
                utils::to_lowercase(key);
                fmt::print("  {:<{}}: ", key, WIDTH);
                for (TagLib::StringList::ConstIterator value_it =
                         it->second.begin();
                     value_it != it->second.end(); ++value_it) {
                    if (!value_it->isEmpty()) {
                        fmt::print("{} ", value_it->toCString(true));
                    }
                }
                fmt::print("\n");
            }
        }
    }
}
