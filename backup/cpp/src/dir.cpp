#include "dir.hpp"
#include <filesystem>
#include <iostream>
#include <system_error> // For more specific error codes

namespace fs = std::filesystem;

void Dir::deleteDirectory(std::string_view path) {
    std::error_code ec;

    if (!fs::exists(path)) {
        // If the path doesn't exist, no need to throw an error
        return;
    }

    if (!fs::is_directory(path)) {
        throw std::invalid_argument(path.data() +
                                    std::string(" is not a directory"));
    }

    // Note: `remove_all` is more convenient here than iterating ourselves
    fs::remove_all(path, ec);
    if (ec) {
        throw std::filesystem::filesystem_error(
            "Failed to delete directory: " + std::string(path), ec);
    }
}

void Dir::createDirectory(std::string_view path) {
    std::error_code ec;
    fs::create_directories(path, ec); // Creates parent directories if needed
    if (ec) {
        throw std::filesystem::filesystem_error(
            "Failed to create directory: " + std::string(path), ec);
    }
}