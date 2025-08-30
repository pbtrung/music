#include <filesystem>
#include <fstream>
#include <iostream>
#include <stacktrace>
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

static void log_trace() {
    auto trace = std::stacktrace::current();
    SPDLOG_TRACE("Stacktrace:");
    for (const auto &entry : trace) {
        SPDLOG_TRACE("  {}", std::to_string(entry));
    }
}

[[noreturn]] static void die(const std::string &msg) {
    if (!msg.empty()) {
        fmt::println("Error: {}", msg);
        auto trace = std::stacktrace::current();
        fmt::println("Stacktrace:");
        for (const auto &entry : trace) {
            fmt::println("  {}", std::to_string(entry));
        }
    }
    std::exit(EXIT_FAILURE);
}

static void init_log(const std::string &file) {
    constexpr size_t max_size = 20 * 1024 * 1024; // 20 MB
    constexpr size_t max_files = 3;

    auto logger =
        spdlog::rotating_logger_mt("file_logger", file, max_size, max_files);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#:%!] %v");
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::trace);
    spdlog::set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
}

static json get_track(const json &config) {
    CosmosDB cosmos(config);
    return cosmos.get_item().value();
}

static std::string download_track(const json &config, json &track) {
    Downloader dl(config, track);
    dl.download_file();

    if (!dl.succeeded()) {
        return "";
    }

    return dl.assemble_file().value();
}

static void push_track(jdz::SpscQueue<json> &queue, json track) {
    while (!queue.try_push(std::move(track))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

static json pop_track(jdz::SpscQueue<json> &queue) {
    json track;
    while (!queue.try_pop(track)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return track;
}

static void print_info(const json &track) {
    if (track.empty()) {
        SPDLOG_TRACE("Empty track");
        return;
    }

    const std::string filename = track.value("filename", "UNKNOWN");
    const std::string album = track["album"]["path"].get<std::string>();
    const std::string name = track.value("track_name", "UNKNOWN");
    const int id = track.value("track_id", 0);
    const int total = track.value("max_value", 0);
    const int cids = track.contains("cids") ? track["cids"].size() : 0;

    fmt::print("PLAYING: {}\n", filename);
    fmt::print("  track: {} / {}\n", Utilities::format_commas(id),
               Utilities::format_commas(total));
    fmt::print("  album: {}\n", album);
    fmt::print("  filename: {}\n", name);

    if (cids == 1) {
        const std::string cid = track.contains("cids") && !track["cids"].empty()
                                    ? track["cids"][0].get<std::string>()
                                    : "UNKNOWN";
        fmt::print("  info: {} -> {}\n", cid, filename);
    } else {
        fmt::print("  info: {} CIDs -> {}\n", cids, filename);
    }
    std::cout.flush();
}

static void cleanup_file(const fs::path &path) {
    if (fs::exists(path)) {
        try {
            fs::remove(path);
            SPDLOG_TRACE("Removed: {}", path.string());
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Remove failed {}: {}", path.string(), e.what());
        }
    }
}

static void cleanup_cid_files(const json &config, const json &track) {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = config["output"].get<std::string>();

    for (size_t i = 0; i < cids.size(); ++i) {
        const auto &cid = cids[i];
        const fs::path path = output_dir / cid;
        if (fs::exists(path)) {
            try {
                fs::remove(path);
                SPDLOG_TRACE("Removed: {}", path.string());
            } catch (const std::exception &e) {
                SPDLOG_TRACE("Remove failed {}: {}", path.string(), e.what());
            }
        }
    }
}

void producer(jdz::SpscQueue<json> &queue, json &config) {
    SPDLOG_TRACE("Start");
    while (true) {
        SPDLOG_TRACE("Loop starts");

        json track;
        try {
            track = get_track(config);

            std::string filename = download_track(config, track);
            if (!filename.empty()) {
                track["filename"] = filename;
                track["max_value"] = config["max_value"].get<int>();
                SPDLOG_TRACE("Push: {}", filename);
                push_track(queue, std::move(track));
            } else {
                SPDLOG_TRACE("Download failed");
            }
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
            cleanup_cid_files(config, track);
            log_trace();
        } catch (...) {
            SPDLOG_TRACE("Unknown error");
            cleanup_cid_files(config, track);
            log_trace();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SPDLOG_TRACE("Loop ends");
    }
}

void consumer(jdz::SpscQueue<json> &queue, const json &config) {
    SPDLOG_TRACE("Start");
    while (true) {
        SPDLOG_TRACE("Loop starts");
        fs::path file_path;
        bool valid_path = false;

        try {
            json track = pop_track(queue);

            const fs::path output_dir = config["output"].get<std::string>();
            const std::string filename = track["filename"].get<std::string>();
            file_path = output_dir / filename;
            valid_path = true;

            SPDLOG_TRACE("Pop: {}", filename);
            print_info(track);

            AudioDecoder decoder(config["pipe_name"].get<std::string>(),
                                 filename, file_path.string());
            decoder.decode();
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
            log_trace();
        } catch (...) {
            SPDLOG_TRACE("Unknown error");
            log_trace();
        }

        if (valid_path) {
            cleanup_file(file_path);
        }
        SPDLOG_TRACE("Loop ends");
    }
}

int main(int argc, char *argv[]) {
    SPDLOG_TRACE("Start");

    int num_files = 4;
    if (argc != 2) {
        die("Usage: <program> <config_file>");
    }

    std::ifstream cfg_stream(argv[1]);
    json config = json::parse(cfg_stream);
    cfg_stream.close();

    init_log(config["log"].get<std::string>());
    num_files = config["num_files"].get<int>();
    fs::remove_all(config["output"].get<std::string>());

    jdz::SpscQueue<json> queue(num_files);
    std::jthread p(producer, std::ref(queue), std::ref(config));
    std::jthread c(consumer, std::ref(queue), std::ref(config));

    p.join();
    c.join();
    return 0;
}
