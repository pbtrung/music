#include "dir.hpp"
#include "fmtlog-inl.hpp"
#include <filesystem>
#include <fmt/core.h>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

void Dir::deleteDirectory(std::string_view path) {
    std::error_code ec;

    if (!fs::exists(path)) {
        return;
    }

    if (!fs::is_directory(path)) {
        loge("'{}' is not a directory", path.data());
        throw std::runtime_error(
            fmt::format("'{}' is not a directory", path.data()));
    }

    fs::remove_all(path, ec);
    if (ec) {
        loge("Failed to delete directory: '{}'", path.data());
        throw std::filesystem::filesystem_error(
            fmt::format("Failed to delete directory: '{}'", path.data()), ec);
    }
}

void Dir::createDirectory(std::string_view path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        loge("Failed to create directory: '{}'", path.data());
        throw std::filesystem::filesystem_error(
            fmt::format("Failed to create directory: '{}'", path.data()), ec);
    }
}