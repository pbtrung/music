#include "dir.hpp"
#include <filesystem>
#include <fmt/core.h>
#include <iostream>
#include <spdlog/spdlog.h>
#include <system_error>

namespace fs = std::filesystem;

void Dir::deleteDirectory(std::string_view path) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    std::error_code ec;

    if (!fs::exists(path)) {
        return;
    }

    if (!fs::is_directory(path)) {
        SPDLOG_LOGGER_ERROR(logger, "Error: This is not a directory: {}",
                            path.data());
        throw std::runtime_error("");
    }

    fs::remove_all(path, ec);
    if (ec) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to delete directory: {}",
                            path.data());
        throw std::runtime_error("");
    }
}

void Dir::createDirectory(std::string_view path) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to create directory: {}",
                            path.data());
        throw std::runtime_error("");
    }
}