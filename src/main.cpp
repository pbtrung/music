#include <fstream>
#include <string>
#include <thread>

#include <fmt/base.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "atomic_queues.hpp"
#include "cosmosdb.hpp"
#include "downloader.hpp"
#include "json.hpp"

using json = nlohmann::json;

[[noreturn]] static void exit_on_error(const std::string &msg) {
    if (!msg.empty()) {
        fmt::println("Error: {}", msg);
    }
    std::exit(EXIT_FAILURE);
}

void setup_logging_to_file(const std::string &log_file) {
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
    for (int i = 0; i < 4; ++i) {
        std::ifstream f(config_file);
        json config = json::parse(f);

        CosmosDB cosmos(config);
        json track = cosmos.get_item().value();

        // Downloader downloader(config, track);
        // downloader.download_file();

        queue.push(std::move(track));
    }
}

void consumer(jdz::SpscQueue<json> &queue, const std::string &config_file) {
    for (int i = 0; i < 8; ++i) {
        json track;
        queue.pop(track);
    }
}

int main(int argc, char *argv[]) {
    int num_files = 4;

    if (argc != 2) {
        exit_on_error("Usage: <program> <config_file>");
    } else {
        std::ifstream config_file(argv[1]);
        json config = json::parse(config_file);
        setup_logging_to_file(config["log"].get<std::string>());
        num_files = config["num_files"].get<int>();
    }

    std::string config_file(argv[1]);
    jdz::SpscQueue<json> queue(num_files);
    std::jthread p(producer, std::ref(queue), std::ref(config_file));
    std::jthread c(consumer, std::ref(queue), std::ref(config_file));

    p.join();
    c.join();

    return 0;
}
