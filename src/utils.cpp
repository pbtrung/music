#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <regex>
#include <unordered_set>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include "cppcodec/base64_url_unpadded.hpp"
#include "utils.hpp"

using base64_url_unpadded = cppcodec::base64_url_unpadded;

namespace {
// Convert bytes to hex string
std::string bytes_to_hex(const std::span<const unsigned char> bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);

    constexpr char hex_chars[] = "0123456789abcdef";
    for (const auto byte : bytes) {
        result += hex_chars[(byte >> 4) & 0x0F];
        result += hex_chars[byte & 0x0F];
    }

    return result;
}
} // namespace

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

std::string Utilities::format_commas(long num) noexcept {
    std::string str = std::to_string(num);

    // Handle negative numbers
    bool negative = (str[0] == '-');
    int start = negative ? 1 : 0;

    // Get the numeric part
    std::string digits = str.substr(start);
    int len = digits.length();

    if (len <= 3) {
        return str; // No commas needed
    }

    std::string result;
    if (negative) {
        result += '-';
    }

    // Calculate how many digits in the first group
    int first_group_size = len % 3;
    if (first_group_size == 0) {
        first_group_size = 3;
    }

    // Add first group
    result += digits.substr(0, first_group_size);

    // Add remaining groups of 3 with commas
    for (int i = first_group_size; i < len; i += 3) {
        result += ',';
        result += digits.substr(i, 3);
    }

    return result;
}

std::string
Utilities::compute_sha3_256(const std::filesystem::path &file_path) noexcept {
    try {
        // Check if file exists and is readable
        if (!std::filesystem::exists(file_path) ||
            !std::filesystem::is_regular_file(file_path)) {
            return "";
        }

        // Open file
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            return "";
        }

        // Create SHA3-256 context
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);

        if (!ctx) {
            return "";
        }

        const EVP_MD *md = EVP_sha3_256();
        if (!md) {
            return "";
        }

        if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
            return "";
        }

        // Read file in chunks and update hash
        constexpr size_t buffer_size = 8192;
        std::array<char, buffer_size> buffer;

        while (file.read(buffer.data(), buffer_size) || file.gcount() > 0) {
            const auto bytes_read = static_cast<size_t>(file.gcount());
            if (EVP_DigestUpdate(ctx.get(), buffer.data(), bytes_read) != 1) {
                return "";
            }
        }

        // Finalize hash
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
        unsigned int hash_len = 0;

        if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &hash_len) != 1) {
            return "";
        }

        // Convert to hex string
        return bytes_to_hex(
            std::span<const unsigned char>(hash.data(), hash_len));

    } catch (...) {
        return "";
    }
}

std::string
Utilities::hmac_sha3_256(std::string_view hmac_key_b64,
                         const std::vector<std::byte> &input) noexcept {
    try {
        // Decode base64 key
        auto key_bytes = base64_url_unpadded::decode(hmac_key_b64);
        if (key_bytes.empty() && !hmac_key_b64.empty()) {
            return "";
        }

        // Create EVP MAC context for HMAC (OpenSSL 3.0+ way)
        std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac(
            EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);

        if (!mac) {
            return "";
        }

        std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> ctx(
            EVP_MAC_CTX_new(mac.get()), EVP_MAC_CTX_free);

        if (!ctx) {
            return "";
        }

        // Set the digest algorithm to SHA3-256
        const char *digest_name = "SHA3-256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_utf8_string("digest", const_cast<char *>(digest_name),
                                   0),
            OSSL_PARAM_END};

        // Initialize MAC with key and parameters
        if (EVP_MAC_init(ctx.get(), key_bytes.data(), key_bytes.size(),
                         params) != 1) {
            return "";
        }

        // Update MAC with input data
        if (!input.empty()) {
            if (EVP_MAC_update(
                    ctx.get(),
                    reinterpret_cast<const unsigned char *>(input.data()),
                    input.size()) != 1) {
                return "";
            }
        }

        // Finalize MAC
        std::array<unsigned char, EVP_MAX_MD_SIZE> hmac_result;
        size_t hmac_len = 0;

        if (EVP_MAC_final(ctx.get(), hmac_result.data(), &hmac_len,
                          hmac_result.size()) != 1) {
            return "";
        }

        // Encode result as base64 without padding
        return base64_url_unpadded::encode(
            std::span<const unsigned char>(hmac_result.data(), hmac_len));

    } catch (...) {
        return "";
    }
}

std::string Utilities::hmac_sha3_256(std::string_view hmac_key_b64,
                                     const std::string &input) noexcept {
    try {
        // Decode base64 key
        auto key_bytes = base64_url_unpadded::decode(hmac_key_b64);
        if (key_bytes.empty() && !hmac_key_b64.empty()) {
            SPDLOG_TRACE("Failed to decode HMAC key");
            return "";
        }

        // Create EVP MAC context for HMAC (OpenSSL 3.0+ way)
        std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac(
            EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);

        if (!mac) {
            SPDLOG_TRACE("Failed to create EVP_MAC");
            return "";
        }

        std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> ctx(
            EVP_MAC_CTX_new(mac.get()), EVP_MAC_CTX_free);

        if (!ctx) {
            SPDLOG_TRACE("Failed to create EVP_MAC_CTX");
            return "";
        }

        // Set the digest algorithm to SHA3-256
        const char *digest_name = "SHA3-256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_utf8_string("digest", const_cast<char *>(digest_name),
                                   0),
            OSSL_PARAM_END};

        // Initialize MAC with key and parameters
        if (EVP_MAC_init(ctx.get(), key_bytes.data(), key_bytes.size(),
                         params) != 1) {
            SPDLOG_TRACE("Failed to initialize EVP_MAC");
            return "";
        }

        // Update MAC with input data (if any)
        if (!input.empty()) {
            if (EVP_MAC_update(
                    ctx.get(),
                    reinterpret_cast<const unsigned char *>(input.data()),
                    input.size()) != 1) {
                SPDLOG_TRACE("Failed to update EVP_MAC with input");
                return "";
            }
        }

        // Finalize MAC
        std::array<unsigned char, EVP_MAX_MD_SIZE> hmac_result;
        size_t hmac_len = 0;

        if (EVP_MAC_final(ctx.get(), hmac_result.data(), &hmac_len,
                          hmac_result.size()) != 1) {
            SPDLOG_TRACE("Failed to finalize EVP_MAC");
            return "";
        }

        // Encode result as base64 without padding
        return base64_url_unpadded::encode(
            std::span<const unsigned char>(hmac_result.data(), hmac_len));

    } catch (const std::exception &e) {
        SPDLOG_TRACE("Exception computing HMAC: {}", e.what());
        return "";
    } catch (...) {
        SPDLOG_TRACE("Unknown exception computing HMAC");
        return "";
    }
}

std::optional<std::uintmax_t>
Utilities::get_file_size(const std::filesystem::path &file_path) noexcept {
    try {
        // Check if file exists and is a regular file
        if (!std::filesystem::exists(file_path) ||
            !std::filesystem::is_regular_file(file_path)) {
            SPDLOG_TRACE("File does not exist or is not a regular file: {}",
                         file_path.string());
            return std::nullopt;
        }

        std::error_code ec;
        auto size = std::filesystem::file_size(file_path, ec);
        if (ec) {
            SPDLOG_TRACE("Error getting file size: {}", ec.message());
            return std::nullopt;
        }

        return size;
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Exception getting file size: {}", e.what());
        return std::nullopt;
    } catch (...) {
        SPDLOG_TRACE("Unknown exception getting file size");
        return std::nullopt;
    }
}

std::string Utilities::hmac_sha3_256_from_file(
    std::string_view hmac_key_b64,
    const std::filesystem::path &file_path) noexcept {
    try {
        // Check if file exists and is readable
        if (!std::filesystem::exists(file_path) ||
            !std::filesystem::is_regular_file(file_path)) {
            SPDLOG_TRACE("File does not exist or is not a regular file: {}",
                         file_path.string());
            return "";
        }

        // Decode base64 key
        auto key_bytes = base64_url_unpadded::decode(hmac_key_b64);
        if (key_bytes.empty() && !hmac_key_b64.empty()) {
            SPDLOG_TRACE("Failed to decode HMAC key");
            return "";
        }

        // Open file
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            SPDLOG_TRACE("Failed to open file: {}", file_path.string());
            return "";
        }

        // Create EVP MAC context for HMAC (OpenSSL 3.0+ way)
        std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac(
            EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);

        if (!mac) {
            SPDLOG_TRACE("Failed to create EVP_MAC");
            return "";
        }

        std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> ctx(
            EVP_MAC_CTX_new(mac.get()), EVP_MAC_CTX_free);

        if (!ctx) {
            SPDLOG_TRACE("Failed to create EVP_MAC_CTX");
            return "";
        }

        // Set the digest algorithm to SHA3-256
        const char *digest_name = "SHA3-256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_utf8_string("digest", const_cast<char *>(digest_name),
                                   0),
            OSSL_PARAM_END};

        // Initialize MAC with key and parameters
        if (EVP_MAC_init(ctx.get(), key_bytes.data(), key_bytes.size(),
                         params) != 1) {
            SPDLOG_TRACE("Failed to initialize EVP_MAC");
            return "";
        }

        // Read file in chunks and update HMAC
        constexpr size_t buffer_size = 8192;
        std::array<char, buffer_size> buffer;

        while (file.read(buffer.data(), buffer_size) || file.gcount() > 0) {
            const auto bytes_read = static_cast<size_t>(file.gcount());
            if (EVP_MAC_update(
                    ctx.get(),
                    reinterpret_cast<const unsigned char *>(buffer.data()),
                    bytes_read) != 1) {
                SPDLOG_TRACE("Failed to update EVP_MAC");
                return "";
            }
        }

        // Check for file read errors
        if (!file.eof() && file.fail()) {
            SPDLOG_TRACE("Error reading file: {}", file_path.string());
            return "";
        }

        // Finalize HMAC
        std::array<unsigned char, EVP_MAX_MD_SIZE> hmac_result;
        size_t hmac_len = 0;

        if (EVP_MAC_final(ctx.get(), hmac_result.data(), &hmac_len,
                          hmac_result.size()) != 1) {
            SPDLOG_TRACE("Failed to finalize EVP_MAC");
            return "";
        }

        // Encode result as base64 without padding
        return base64_url_unpadded::encode(
            std::span<const unsigned char>(hmac_result.data(), hmac_len));

    } catch (const std::exception &e) {
        SPDLOG_TRACE("Exception computing HMAC from file: {}", e.what());
        return "";
    } catch (...) {
        SPDLOG_TRACE("Unknown exception computing HMAC from file");
        return "";
    }
}