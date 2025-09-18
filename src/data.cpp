#include <fstream>
#include <iostream>
#include <string>

#include <fmt/base.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "json.hpp"

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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        die("Usage: <config.json> <command>");
    }

    std::string config_file = argv[1];
    std::string command = argv[2];

    std::ifstream cfg_stream(config_file);
    json config = json::parse(cfg_stream);
    cfg_stream.close();

    init_log(config["log"].get<std::string>());

    if (command == "migrate") {
        SPDLOG_TRACE("Running migration with {}", config_file);
    } else if (command == "upload-music") {
        SPDLOG_TRACE("Running upload-music with {}", config_file);
    } else if (command == "upload-data") {
        SPDLOG_TRACE("Running upload-data with {}", config_file);
    } else {
        die("Unknown command");
    }

    return 0;
}
