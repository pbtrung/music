#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "audio_processor.hpp"

int main(int argc, char *argv[]) {
    // Set up logging
    spdlog::set_level(spdlog::level::trace);
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <audio_file1> [audio_file2] ...\n";
        std::cerr << "Example: " << argv[0]
                  << " song.mp3 track.flac album.m4a\n";
        return 1;
    }

    std::vector<std::filesystem::path> audio_files;

    // Collect all audio files from command line arguments
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path file_path(argv[i]);

        if (!std::filesystem::exists(file_path)) {
            std::cerr << "Error: File does not exist: " << file_path << "\n";
            continue;
        }

        if (!std::filesystem::is_regular_file(file_path)) {
            std::cerr << "Error: Not a regular file: " << file_path << "\n";
            continue;
        }

        audio_files.push_back(file_path);
    }

    if (audio_files.empty()) {
        std::cerr << "Error: No valid audio files provided\n";
        return 1;
    }

    std::cout << "Processing " << audio_files.size() << " audio file(s)...\n\n";

    // Process each file
    for (const auto &file_path : audio_files) {
        std::cout << "Processing: " << file_path.filename() << "\n";
        std::cout << "  Path: " << file_path << "\n";

        try {
            double track_gain =
                TrackGainAnalyzer::compute_and_write_track_gain(file_path);

            std::cout << "Track Gain: " << std::format("{:.2f} dB", track_gain)
                      << "\n";
            std::cout << "R128_TRACK_GAIN tag written successfully\n";
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n";
        }

        std::cout << "\n";
    }

    std::cout << "Processing complete!\n";
    return 0;
}