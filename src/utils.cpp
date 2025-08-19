#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <limits>
#include <ranges>
#include <regex>
#include <unordered_set>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include "utils.hpp"

bool Utilities::validate_string(std::string_view str, size_t max_len) noexcept {
    if (str.empty() || str.length() > max_len) {
        SPDLOG_TRACE("Invalid string length: {}", str.length());
        return false;
    }
    return true;
}

bool Utilities::validate_path(std::string_view component) noexcept {
    if (component.empty()) {
        return false;
    }
    const std::array<std::string_view, 3> dangerous_patterns = {"..", "/",
                                                                "\\"};
    for (const auto &pattern : dangerous_patterns) {
        if (component.find(pattern) != std::string_view::npos) {
            SPDLOG_TRACE("Invalid path component: {}", component);
            return false;
        }
    }
    return true;
}

bool Utilities::validate_filename(std::string_view filename) noexcept {
    if (!validate_string(filename, max_filename_length)) {
        return false;
    }
    constexpr std::string_view invalid_chars = "<>:\"|?*";
    for (char c : filename) {
        if (invalid_chars.find(c) != std::string_view::npos) {
            SPDLOG_TRACE("Invalid character '{}' in filename", c);
            return false;
        }
    }
    return true;
}

void Utilities::trim_spaces(std::string &str) noexcept {
    if (str.empty()) {
        SPDLOG_TRACE("Empty string");
        return;
    }
    auto start = str.find_first_not_of(' ');
    if (start == std::string::npos) {
        str.clear();
        return;
    }
    auto end = str.find_last_not_of(' ');
    str = str.substr(start, end - start + 1);

    std::string result;
    result.reserve(str.length());
    bool prev_was_space = false;
    for (char c : str) {
        if (c == ' ') {
            if (!prev_was_space) {
                result += c;
                prev_was_space = true;
            }
        } else {
            result += c;
            prev_was_space = false;
        }
    }
    str = std::move(result);
}

std::string Utilities::format_time(int seconds) noexcept {
    if (seconds < 0) {
        SPDLOG_TRACE("Negative seconds value: {}", seconds);
        return "00:00";
    }
    constexpr int max_seconds = std::numeric_limits<int>::max() / 3600;
    if (seconds > max_seconds) {
        SPDLOG_TRACE("Seconds value too large: {}", seconds);
        return "99:59:59";
    }
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int remaining_seconds = seconds % 60;
    if (hours > 0) {
        return std::format("{:02d}:{:02d}:{:02d}", hours, minutes,
                           remaining_seconds);
    } else {
        return std::format("{:02d}:{:02d}", minutes, remaining_seconds);
    }
}

std::optional<std::filesystem::path>
Utilities::make_path(std::string_view directory,
                     std::string_view filename) noexcept {
    if (!validate_string(directory, max_path_length) ||
        !validate_filename(filename)) {
        SPDLOG_TRACE("Invalid input parameters");
        return std::nullopt;
    }
    if (!validate_path(filename)) {
        SPDLOG_TRACE("Invalid filename component");
        return std::nullopt;
    }
    if (directory.length() + filename.length() + 1 > max_path_length) {
        SPDLOG_TRACE("Path too long");
        return std::nullopt;
    }
    try {
        std::filesystem::path result{directory};
        result /= filename;
        return result;
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Exception creating path: {}", e.what());
        return std::nullopt;
    }
}

void Utilities::to_lower(std::string &str) noexcept {
    std::ranges::transform(str, str.begin(),
                           [](unsigned char c) { return std::tolower(c); });
}

std::optional<std::string>
Utilities::get_extension(std::string_view filename) noexcept {
    if (!validate_string(filename, max_filename_length)) {
        SPDLOG_TRACE("Invalid input filename");
        return std::nullopt;
    }
    try {
        const std::regex ext_pattern{R"(.*\.(opus|mp3|m4a|m4b)$)",
                                     std::regex::icase | std::regex::optimize};
        std::match_results<std::string_view::const_iterator> match;
        if (std::regex_match(filename.begin(), filename.end(), match,
                             ext_pattern)) {
            if (match.size() >= 2) {
                std::string ext{match[1].first, match[1].second};
                to_lower(ext);
                if (ext.length() > max_extension_length) {
                    SPDLOG_TRACE("Extension too long: {}", ext.length());
                    return std::nullopt;
                }
                return ext;
            }
        }
        SPDLOG_TRACE("No valid extension found in: {}", filename);
        return std::nullopt;
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Regex exception: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::string>
Utilities::generate_filename(std::string_view original_filename) noexcept {
    if (!validate_string(original_filename, max_filename_length)) {
        SPDLOG_TRACE("Invalid input filename");
        return std::nullopt;
    }
    auto ext = get_extension(original_filename);
    if (!ext) {
        SPDLOG_TRACE("Failed to get extension");
        return std::nullopt;
    }
    auto random_part = generate_random_string(default_filename_length);
    if (random_part.empty()) {
        SPDLOG_TRACE("Failed to generate random filename");
        return std::nullopt;
    }
    std::string result = std::format("{}.{}", random_part, *ext);
    if (result.length() > max_filename_length) {
        SPDLOG_TRACE("Generated filename too long");
        return std::nullopt;
    }
    return result;
}

std::optional<std::vector<int>>
Utilities::generate_unique_ints(int count, int min_val, int max_val) noexcept {
    if (count <= 0) {
        SPDLOG_TRACE("Invalid count: {}", count);
        return std::nullopt;
    }
    if (min_val > max_val) {
        SPDLOG_TRACE("Invalid range: min={}, max={}", min_val, max_val);
        return std::nullopt;
    }
    const int64_t range =
        static_cast<int64_t>(max_val) - static_cast<int64_t>(min_val) + 1;
    if (count > range) {
        SPDLOG_TRACE("Count ({}) exceeds range ({})", count, range);
        return std::nullopt;
    }

    std::vector<int> result;
    result.reserve(count);
    std::unordered_set<int> generated;

    while (static_cast<int>(result.size()) < count) {
        uint64_t value{};
        if (RAND_bytes(reinterpret_cast<unsigned char *>(&value),
                       sizeof(value)) != 1) {
            SPDLOG_TRACE("RAND_bytes failed");
            return std::nullopt;
        }
        value =
            min_val + (value % (static_cast<uint64_t>(max_val - min_val + 1)));
        if (generated.insert(static_cast<int>(value)).second) {
            result.push_back(static_cast<int>(value));
        }
    }
    return result;
}

std::string Utilities::generate_random_string(size_t length) noexcept {
    if (length < min_random_string_length ||
        length > max_random_string_length) {
        SPDLOG_TRACE("Invalid length: {}", length);
        return "";
    }
    constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    std::string result;
    result.reserve(length);

    for (size_t i = 0; i < length; ++i) {
        uint64_t byte{};
        if (RAND_bytes(reinterpret_cast<unsigned char *>(&byte),
                       sizeof(byte)) != 1) {
            SPDLOG_TRACE("RAND_bytes failed");
            return "";
        }
        result += alphabet[byte % alphabet.size()];
    }
    return result;
}
