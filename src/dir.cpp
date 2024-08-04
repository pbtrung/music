#include "dir.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

void dir::delete_directory(const std::string &path) {
    try {
        // Check if the path exists and is a directory
        if (fs::exists(path) && fs::is_directory(path)) {
            // Iterate through the directory contents
            for (const auto &entry : fs::directory_iterator(path)) {
                if (fs::is_directory(entry.status())) {
                    // Recursively delete subdirectory
                    delete_directory(entry.path().string());
                } else {
                    // Delete file
                    fs::remove(entry.path());
                }
            }
            // Delete the directory itself
            fs::remove(path);
        }
    } catch (const fs::filesystem_error &e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
        throw; // Re-throw to propagate the error
    }
}

void dir::create_directory(const std::string &path) {
    try {
        if (!fs::create_directory(path)) {
            throw std::runtime_error("Failed to create directory: " + path);
        }
    } catch (const fs::filesystem_error &e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
        throw; // Re-throw to propagate the error
    }
}
