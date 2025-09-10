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
#include "downloader.hpp"
#include "json.hpp"
#include "track.hpp"
#include "utils.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

[[noreturn]] static void die(const std::string &msg) {
    if (!msg.empty()) {
        fmt::println("Error: {}", msg);
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

static json get_track(jdz::SpscQueue<json> &queue, const json &config) {
    int min_value;
    int max_value;
    const auto rand_range = Utilities::generate_unique_ints(1, 0, 1);
    const auto &ranges = config["ranges"];

    if (queue.size() <= 5) {
        if (rand_range->front() == 0) {
            const auto &r = ranges["low"].get<std::vector<int>>();
            min_value = r[0];
            max_value = r[1];
        } else {
            const auto &r = ranges["high"].get<std::vector<int>>();
            min_value = r[0];
            max_value = r[1];
        }
    } else {
        const auto &r = ranges["middle"].get<std::vector<int>>();
        min_value = r[0];
        max_value = r[1];
    }

    const auto rand_num =
        Utilities::generate_unique_ints(1, min_value, max_value);
    std::string query = fmt::format("SELECT * FROM tracks WHERE track_id = {}",
                                    rand_num->front());
    SPDLOG_TRACE("{}", query);

    const std::string url =
        fmt::format("https://{}.r2.cloudflarestorage.com/{}/{}",
                    config["r2"]["account_id"].get<std::string>(),
                    config["r2"]["bucket"].get<std::string>(),
                    config["r2"]["db_file"].get<std::string>());
    Track track = Track::load(url, config, query);
    return track.get_json();
}

static std::string download_track(json &config, const json &track) {
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
            track = get_track(queue, config);

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
        } catch (...) {
            SPDLOG_TRACE("Unknown error");
        }

        if (track.contains("cids") && config.contains("output")) {
            cleanup_cid_files(config, track);
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

            AudioDecoder decoder(config["pipe"].get<std::string>(), filename,
                                 file_path.string());
            decoder.decode();
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
        } catch (...) {
            SPDLOG_TRACE("Unknown error");
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
