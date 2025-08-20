#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <fmt/base.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "atomic_queues.hpp"
#include "audio_decoder.hpp"
#include "cosmosdb.hpp"
#include "downloader.hpp"
#include "json.hpp"
#include "utils.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

[[noreturn]] static void exit_on_error(const std::string &msg) {
    if (!msg.empty()) {
        fmt::println("Error: {}", msg);
    }
    std::exit(EXIT_FAILURE);
}

static void setup_logging_to_file(const std::string &log_file) {
    // 20 MB
    constexpr size_t max_size = 20 * 1024 * 1024;
    constexpr size_t max_files = 3;

    auto file_logger = spdlog::rotating_logger_mt("file_logger", log_file,
                                                  max_size, max_files);

    file_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#:%!] %v");

    file_logger->set_level(spdlog::level::trace);
    spdlog::set_level(spdlog::level::trace);

    file_logger->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(file_logger);
}

void producer(jdz::SpscQueue<json> &queue, const std::string &config_file) {
    SPDLOG_TRACE("Start");
    while (true) {
        SPDLOG_TRACE("Start loop");
        try {
            std::ifstream f(config_file);
            json config = json::parse(f);

            CosmosDB cosmos(config);
            json track = cosmos.get_item().value();

            Downloader downloader(config, track);
            downloader.download_file();

            if (downloader.succeeded()) {
                track["filename"] = downloader.assemble_file().value();
                track["max_value"] = config["max_value"].get<int>();
                SPDLOG_TRACE("Push: {}", track["filename"].get<std::string>());
                queue.push(std::move(track));
            } else {
                SPDLOG_TRACE("Failed to download file");
            }
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
        }
        SPDLOG_TRACE("End loop");
    }
    SPDLOG_TRACE("End");
}

static void print_track_info(const json &track) {
    if (track.empty()) {
        SPDLOG_TRACE("Empty track");
        return;
    }

    // Extract values with safe defaults
    const std::string filename = track.value("filename", "UNKNOWN");
    const std::string album_path = track["album"]["path"].get<std::string>();
    const std::string track_name = track.value("track_name", "UNKNOWN");
    const int track_id = track.value("track_id", 0);
    const int num_tracks = track.value("max_value", 0);
    const int num_cids = track.contains("cids") ? track["cids"].size() : 0;
    static constexpr size_t width = 1;

    std::string track_id_str = Utilities::format_commas(track_id);
    std::string num_tracks_str = Utilities::format_commas(num_tracks);

    // Print track information using fmt
    fmt::print("PLAYING: {}\n", filename);
    fmt::print("  {:<{}}: {} / {}\n", "track", width, track_id_str,
               num_tracks_str);
    fmt::print("  {:<{}}: {}\n", "album", width, album_path);
    fmt::print("  {:<{}}: {}\n", "filename", width, track_name);

    if (num_cids == 1) {
        const std::string cid = track.contains("cids") && !track["cids"].empty()
                                    ? track["cids"][0].get<std::string>()
                                    : "UNKNOWN";
        fmt::print("  {:<{}}: {} -> {}\n", "info", width, cid, filename);
    } else {
        fmt::print("  {:<{}}: {} CIDs -> {}\n", "info", width, num_cids,
                   filename);
    }

    std::cout.flush();
}

void consumer(jdz::SpscQueue<json> &queue, const std::string &config_file) {
    SPDLOG_TRACE("Start");
    while (true) {
        SPDLOG_TRACE("Start loop");
        try {
            std::ifstream f(config_file);
            json config = json::parse(f);

            json track;
            queue.pop(track);

            const fs::path output_dir = config["output"].get<std::string>();
            const std::string filename = track["filename"].get<std::string>();
            const fs::path file_path = output_dir / filename;

            SPDLOG_TRACE("Pop: {}", filename);
            print_track_info(track);
            AudioDecoder audio_decoder(config["pipe_name"].get<std::string>(),
                                       filename, file_path.string());
            audio_decoder.decode();
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
        }
        SPDLOG_TRACE("End loop");
    }
    SPDLOG_TRACE("End");
}

int main(int argc, char *argv[]) {
    int num_files = 4;

    SPDLOG_TRACE("Start");
    if (argc != 2) {
        exit_on_error("Usage: <program> <config_file>");
    } else {
        std::ifstream config_file(argv[1]);
        json config = json::parse(config_file);

        setup_logging_to_file(config["log"].get<std::string>());
        num_files = config["num_files"].get<int>();
        fs::remove_all(config["output"].get<std::string>());
    }
    SPDLOG_TRACE("Empty track");

    std::string config_file(argv[1]);
    jdz::SpscQueue<json> queue(num_files);
    std::jthread p(producer, std::ref(queue), std::ref(config_file));
    std::jthread c(consumer, std::ref(queue), std::ref(config_file));

    p.join();
    c.join();

    SPDLOG_TRACE("End");

    return 0;
}
