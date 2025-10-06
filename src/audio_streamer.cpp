#include <fmt/base.h>
#include <iostream>
#include <spdlog/spdlog.h>

#include "audio_processor.hpp"
#include "audio_streamer.hpp"
#include "downloader.hpp"
#include "track.hpp"
#include "utils.hpp"

AudioStreamManager::AudioStreamManager(const json &cfg, int queue_size)
    : queue(queue_size), config(cfg) {
    SPDLOG_TRACE("AudioStreamManager constructed with queue size: {}",
                 queue_size);
}

AudioStreamManager::~AudioStreamManager() {
    stop();
}

void AudioStreamManager::start() {
    SPDLOG_TRACE("Starting AudioStreamManager");

    // Clean up output directory
    fs::remove_all(config["output"].get<std::string>());

    // Start producer and consumer threads
    producer_thread = std::jthread([this]() { producer_loop(); });
    consumer_thread = std::jthread([this]() { consumer_loop(); });

    SPDLOG_TRACE("AudioStreamManager started");
}

void AudioStreamManager::stop() {
    SPDLOG_TRACE("Stopping AudioStreamManager");

    if (producer_thread.joinable()) {
        producer_thread.request_stop();
    }
    if (consumer_thread.joinable()) {
        consumer_thread.request_stop();
    }

    SPDLOG_TRACE("AudioStreamManager stopped");
}

void AudioStreamManager::wait() {
    if (producer_thread.joinable()) {
        producer_thread.join();
    }
    if (consumer_thread.joinable()) {
        consumer_thread.join();
    }
}

size_t AudioStreamManager::get_queue_size() const {
    return queue.size();
}

json AudioStreamManager::get_track() {
    const auto rand_range = Utilities::generate_unique_ints(1, 0, 1);

    std::string range_category;
    if (queue.size() <= 6) {
        range_category = (rand_range->front() == 0) ? "low" : "high";
    } else {
        range_category = "all";
    }
    const auto &ranges = config["ranges"];
    const auto &r = ranges[range_category].get<std::vector<int>>();
    SPDLOG_TRACE(
        "Range selection: queue.size={}, rand_range={}, range_category='{}', range=[{}, {}]",
        queue.size(), rand_range->front(), range_category, r[0], r[1]);

    const auto rand_num = Utilities::generate_unique_ints(1, r[0], r[1]);
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

std::string AudioStreamManager::download_track(const json &track) {
    Downloader dl(config, track);
    dl.download_file();

    if (!dl.succeeded()) {
        return "";
    }

    return dl.assemble_file().value();
}

void AudioStreamManager::push_track(json track) {
    while (!queue.try_push(std::move(track))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

int AudioStreamManager::compute_track_gain(const json &config,
                                           const json &track) {
    double gain_db = -7.0;
    try {
        const fs::path output_dir = config["output"].get<std::string>();
        const std::string filename = track["filename"].get<std::string>();
        const fs::path file_path = output_dir / filename;

        TrackGainAnalyzer analyzer(file_path);
        gain_db = analyzer.compute_track_gain();
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Error: {}", e.what());
    } catch (...) {
        SPDLOG_TRACE("Unknown error");
    }

    double gain_multiplier = std::pow(10.0, gain_db / 20.0);
    int gain_fixed = static_cast<int>(gain_multiplier * 32768.0);
    return gain_fixed;
}

void AudioStreamManager::producer_loop() {
    SPDLOG_TRACE("Producer start");
    while (true) {
        SPDLOG_TRACE("Producer loop starts");

        json track;
        try {
            track = get_track();

            std::string filename = download_track(track);
            if (!filename.empty()) {
                track["filename"] = filename;
                track["max_value"] = config["max_value"].get<int>();
                // track["gain_fixed"] = compute_track_gain(config, track);
                SPDLOG_TRACE("Push: {}", filename);
                push_track(std::move(track));
            } else {
                SPDLOG_TRACE("Download failed");
            }
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Producer error: {}", e.what());
            spdlog::dump_backtrace();
        } catch (...) {
            SPDLOG_TRACE("Producer unknown error");
            spdlog::dump_backtrace();
        }

        if (track.contains("cids") && config.contains("output")) {
            cleanup_cid_files(track);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SPDLOG_TRACE("Producer loop ends");
    }
}

json AudioStreamManager::pop_track() {
    json track;
    while (!queue.try_pop(track)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return track;
}

void AudioStreamManager::print_info(const json &track) {
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

void AudioStreamManager::consumer_loop() {
    SPDLOG_TRACE("Consumer start");
    while (true) {
        SPDLOG_TRACE("Consumer loop starts");
        fs::path file_path;
        bool valid_path = false;

        try {
            json track = pop_track();

            const fs::path output_dir = config["output"].get<std::string>();
            const std::string filename = track["filename"].get<std::string>();
            file_path = output_dir / filename;
            valid_path = true;

            SPDLOG_TRACE("Pop: {}", filename);
            print_info(track);

            int gain_fixed = 0;
            AudioDecoder decoder(config["pipe"].get<std::string>(), file_path,
                                 gain_fixed);
            decoder.decode();
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Consumer error: {}", e.what());
            spdlog::dump_backtrace();
        } catch (...) {
            SPDLOG_TRACE("Consumer unknown error");
            spdlog::dump_backtrace();
        }

        if (valid_path) {
            cleanup_file(file_path);
        }
        SPDLOG_TRACE("Consumer loop ends");
    }
}

void AudioStreamManager::cleanup_file(const fs::path &path) {
    if (fs::exists(path)) {
        try {
            fs::remove(path);
            SPDLOG_TRACE("Removed: {}", path.string());
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Remove failed {}: {}", path.string(), e.what());
            spdlog::dump_backtrace();
        }
    }
}

void AudioStreamManager::cleanup_cid_files(const json &track) {
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
                spdlog::dump_backtrace();
            }
        }
    }
}