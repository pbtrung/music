#include <cstdlib>
#include <format>
#include <iostream>
#include <string>

#include <fmt/base.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "atomic_queues.hpp"
#include "json.hpp"
#include "thread_pool.hpp"

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

    file_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    file_logger->set_level(spdlog::level::trace);
    spdlog::set_level(spdlog::level::trace);

    file_logger->flush_on(spdlog::level::trace);

    spdlog::set_default_logger(file_logger);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        exit_on_error("Usage: <program> <config_file>");
    }

    fmt::println("Hello, World!");

    setup_logging_to_file(argv[1]);
    spdlog::trace("Logging initialized successfully.");

    return 0;
}
