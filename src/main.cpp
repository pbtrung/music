#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <fmt/base.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "audio_streamer.hpp"
#include "json.hpp"

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
    spdlog::enable_backtrace(32);
    spdlog::set_default_logger(logger);
}

int main(int argc, char *argv[]) {
    SPDLOG_TRACE("Start");

    if (argc != 2) {
        die("Usage: <program> <config_file>");
    }

    std::ifstream cfg_stream(argv[1]);
    json config = json::parse(cfg_stream);
    cfg_stream.close();

    init_log(config["log"].get<std::string>());
    int num_files = config["num_files"].get<int>();

    AudioStreamManager stream_manager(config, num_files);
    stream_manager.start();
    stream_manager.wait();

    return 0;
}
