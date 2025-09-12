#include <fstream>
#include <iostream>

#include <fmt/base.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "atomic_queues.hpp"
#include "json.hpp"
#include "track.hpp"

using json = nlohmann::json;

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
    spdlog::enable_backtrace(32);
    spdlog::set_default_logger(logger);
}

static json get_track(jdz::SpscQueue<json> &queue, const json &config,
                      size_t i) {
    std::string query =
        fmt::format("SELECT * FROM tracks WHERE track_id = {}", i);
    SPDLOG_TRACE("{}", query);

    const std::string url =
        fmt::format("https://{}.r2.cloudflarestorage.com/{}/{}",
                    config["r2"]["account_id"].get<std::string>(),
                    config["r2"]["bucket"].get<std::string>(),
                    config["r2"]["db_file"].get<std::string>());
    Track track = Track::load(url, config, query);
    return track.get_json();
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

void producer(jdz::SpscQueue<json> &queue, json &config) {
    SPDLOG_TRACE("Start");
    for (size_t i = 1; i <= 3; i++) {
        SPDLOG_TRACE("Loop starts");

        json track;
        try {
            track = get_track(queue, config, i);
            SPDLOG_TRACE("Push: {}", track["track_name"].get<std::string>());
            push_track(queue, std::move(track));
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
            spdlog::dump_backtrace();
        } catch (...) {
            SPDLOG_TRACE("Unknown error");
            spdlog::dump_backtrace();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SPDLOG_TRACE("Loop ends");
    }

    json track = nullptr;
    push_track(queue, std::move(track));
}

void consumer(jdz::SpscQueue<json> &queue, const json &config) {
    SPDLOG_TRACE("Start");
    while (true) {
        SPDLOG_TRACE("Loop starts");
        try {
            json track = pop_track(queue);
            if (!track.contains("track_name"))
                break;

            SPDLOG_TRACE("Pop: {}", track["track_name"].get<std::string>());
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Error: {}", e.what());
            spdlog::dump_backtrace();
        } catch (...) {
            SPDLOG_TRACE("Unknown error");
            spdlog::dump_backtrace();
        }
        SPDLOG_TRACE("Loop ends");
    }
}

int main(int argc, char *argv[]) {
    int num_files = 4;
    if (argc != 2) {
        die("Usage: <program> <config_file>");
    }

    std::ifstream cfg_stream(argv[1]);
    json config = json::parse(cfg_stream);
    cfg_stream.close();

    init_log(config["log"].get<std::string>());
    num_files = config["num_files"].get<int>();

    jdz::SpscQueue<json> queue(num_files);
    std::jthread p(producer, std::ref(queue), std::ref(config));
    std::jthread c(consumer, std::ref(queue), std::ref(config));

    return 0;
}
