#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "curl.hpp"
#include "gdr_downloader.hpp"
#include "json.hpp"

using json = nlohmann::json;
using namespace std::chrono;
namespace fs = std::filesystem;

GDrDownloader::GDrDownloader(const json &config) : config(config) {
    validate_config();
}

void GDrDownloader::download_file_range(const std::string &file_id,
                                        std::size_t start, std::size_t end,
                                        const fs::path &output_path) {
    auto access_token = get_access_token();

    SPDLOG_TRACE("Downloading file ID: {}", file_id);
    SPDLOG_TRACE("Bytes range: {}-{}", start, end);
    SPDLOG_TRACE("Output path: {}", output_path.string());

    // Ensure output directory exists
    fs::create_directories(output_path.parent_path());

    const fs::path temp_path = fs::path(output_path).concat(".tmp");

    std::ofstream output_file(temp_path, std::ios::binary);
    if (!output_file) {
        throw std::runtime_error(
            fmt::format("Could not open output file: {}", temp_path.string()));
    }

    const int max_retries = config.value("max_retries", 3);
    const int timeout = config.value("timeout", 300);

    try {
        if (attempt_download(file_id, start, end, output_file, max_retries,
                             timeout)) {
            output_file.close();

            // Atomic rename on success
            std::error_code ec;
            fs::rename(temp_path, output_path, ec);
            if (ec) {
                throw std::runtime_error(fmt::format(
                    "Failed to rename {} to {}: {}", temp_path.string(),
                    output_path.string(), ec.message()));
            }

            auto file_size = fs::file_size(output_path);
            SPDLOG_TRACE("Download complete: {} ({} bytes)",
                         output_path.string(), file_size);
        } else {
            output_file.close();
            std::error_code ec;
            fs::remove(temp_path, ec); // Clean up temp file
            throw std::runtime_error(
                fmt::format("Download failed after {} attempts", max_retries));
        }
    } catch (const std::exception &e) {
        output_file.close();
        std::error_code ec;
        // Clean up temp file
        fs::remove(temp_path, ec);
        throw;
    }
}

void GDrDownloader::download_file(const std::string &file_id,
                                  const fs::path &output_path) {
    // For simplicity, download as much as possible (Google Drive will handle
    // range automatically)
    download_file_range(file_id, 0, SIZE_MAX, output_path);
}

bool GDrDownloader::attempt_download(const std::string &file_id,
                                     std::size_t start, std::size_t end,
                                     std::ofstream &output_file,
                                     int max_retries, int timeout) {
    Curl curl;
    curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
    curl.set_option(CURLOPT_USERAGENT, "GDrDownloader/1.0");
    curl.set_file_output(&output_file);

    const std::string url = fmt::format(
        "https://www.googleapis.com/drive/v3/files/{}?alt=media", file_id);
    curl.set_option(CURLOPT_URL, url);
    curl.set_option(CURLOPT_TIMEOUT, timeout);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        // Get fresh access token for each attempt in case it expired
        auto access_token = get_access_token();

        curl.clear_headers();
        curl.set_header(fmt::format("Authorization: Bearer {}", access_token));

        if (end != SIZE_MAX) {
            curl.set_header(fmt::format("Range: bytes={}-{}", start, end));
        }

        SPDLOG_TRACE("Downloading file {} (attempt {}/{})", file_id,
                     attempt + 1, max_retries);

        auto start_time = steady_clock::now();
        int curl_result = curl.perform();
        auto end_time = steady_clock::now();

        if (curl_result == CURLE_OK) {
            auto response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);

            if ((response_code == 200 || response_code == 206)) {
                auto download_time =
                    duration_cast<duration<double>>(end_time - start_time);
                SPDLOG_TRACE("Download completed for file {} in {:.3f}s",
                             file_id, download_time.count());
                return true;
            }

            SPDLOG_TRACE("Invalid response for file {}: HTTP {}", file_id,
                         response_code);
        } else {
            SPDLOG_TRACE(
                "cURL error for file {}: {}", file_id,
                curl_easy_strerror(static_cast<CURLcode>(curl_result)));
        }

        // Reset file position for retry
        reset_output_file(output_file);

        // Add delay between retries (exponential backoff)
        if (attempt < max_retries - 1) {
            auto delay =
                milliseconds(1000 * (1 << attempt)); // 1s, 2s, 4s, 8s...
            std::this_thread::sleep_for(delay);
        }
    }

    SPDLOG_TRACE("Download failed for file {} after {} attempts", file_id,
                 max_retries);
    return false;
}

void GDrDownloader::update_config(const json &new_config) {
    config = new_config;
    validate_config();
}

const json &GDrDownloader::get_config() const noexcept {
    return config;
}

void GDrDownloader::validate_config() const {
    const std::vector<std::string> required_fields = {
        "client_id", "client_secret", "refresh_token"};

    for (const auto &field : required_fields) {
        if (!config.contains(field) ||
            config[field].get<std::string>().empty()) {
            throw std::runtime_error(fmt::format(
                "Missing or empty required config field: {}", field));
        }
    }
}

std::string GDrDownloader::get_access_token() {
    auto now = system_clock::now();

    // Check if we have a cached token that's still valid
    if (config.contains("access_token") && config.contains("expiry_iso")) {
        try {
            auto expiry =
                iso8601_to_timestamp(config["expiry_iso"].get<std::string>());
            if (expiry > now + minutes(1)) { // 1 minute buffer
                SPDLOG_TRACE("Using cached access token (valid until {})",
                             config["expiry_iso"].get<std::string>());
                return config["access_token"].get<std::string>();
            }
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Failed to parse expiry time, refreshing token: {}",
                         e.what());
        }
    }

    return refresh_access_token();
}

std::string GDrDownloader::refresh_access_token() {
    SPDLOG_TRACE("Refreshing Google Drive access token...");

    const int max_retries = config.value("max_retries", 3);
    const int timeout = config.value("timeout", 30);

    const std::string url = "https://oauth2.googleapis.com/token";
    const std::string data = fmt::format(
        "client_id={}&client_secret={}&refresh_token={}&grant_type=refresh_token",
        config["client_id"].get<std::string>(),
        config["client_secret"].get<std::string>(),
        config["refresh_token"].get<std::string>());

    Curl curl;
    curl.reset_string_output();
    curl.set_option(CURLOPT_URL, url);
    curl.set_option(CURLOPT_POSTFIELDS, data);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        curl.clear_headers();
        curl.set_option(CURLOPT_TIMEOUT, timeout);
        curl.set_header("Content-Type: application/x-www-form-urlencoded");

        SPDLOG_TRACE("Token refresh attempt {}/{}", attempt + 1, max_retries);

        auto result = curl.perform();

        if (result == CURLE_OK) {
            auto response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);

            if (response_code == 200) {
                json token_data;
                try {
                    token_data = json::parse(curl.get_response());
                } catch (const json::exception &e) {
                    SPDLOG_TRACE(
                        "Invalid JSON response during token refresh attempt {}: {}",
                        attempt + 1, e.what());
                    if (attempt < max_retries - 1) {
                        auto delay = milliseconds(1000 * (1 << attempt));
                        std::this_thread::sleep_for(delay);
                        continue;
                    }
                    throw std::runtime_error(fmt::format(
                        "Invalid JSON response during token refresh: {}",
                        e.what()));
                }

                if (!token_data.contains("access_token")) {
                    SPDLOG_TRACE(
                        "Missing access_token in response, attempt {}: {}",
                        attempt + 1, curl.get_response());
                    if (attempt < max_retries - 1) {
                        auto delay = milliseconds(1000 * (1 << attempt));
                        std::this_thread::sleep_for(delay);
                        continue;
                    }
                    throw std::runtime_error(fmt::format(
                        "Failed to get access_token: {}", curl.get_response()));
                }

                std::string access_token =
                    token_data["access_token"].get<std::string>();
                int expires_in = token_data.value("expires_in", 3600);
                auto expiry = system_clock::now() +
                              seconds(expires_in - 30); // 30 second buffer
                std::string expiry_iso = timestamp_to_iso8601(expiry);

                // Update config with new token
                config["access_token"] = access_token;
                config["expiry_iso"] = expiry_iso;

                SPDLOG_TRACE("Got new access token, expires at {}", expiry_iso);
                return access_token;
            }

            SPDLOG_TRACE("Token refresh failed, HTTP {} on attempt {}: {}",
                         response_code, attempt + 1, curl.get_response());
        } else {
            SPDLOG_TRACE("cURL error during token refresh attempt {}: {}",
                         attempt + 1,
                         curl_easy_strerror(static_cast<CURLcode>(result)));
        }

        // Add delay between retries (exponential backoff)
        if (attempt < max_retries - 1) {
            auto delay =
                milliseconds(1000 * (1 << attempt)); // 1s, 2s, 4s, 8s...
            SPDLOG_TRACE("Retrying token refresh in {}ms", delay.count());
            std::this_thread::sleep_for(delay);
        }
    }

    throw std::runtime_error(
        fmt::format("Token refresh failed after {} attempts", max_retries));
}

void GDrDownloader::reset_output_file(std::ofstream &output_file) const {
    output_file.clear();
    output_file.seekp(0, std::ios::beg);
}

std::string
GDrDownloader::timestamp_to_iso8601(const system_clock::time_point &tp) {
    auto time_t = system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&time_t, &tm);

    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}+00:00",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

system_clock::time_point
GDrDownloader::iso8601_to_timestamp(const std::string &iso_string) {
    std::tm tm{};
    std::istringstream ss(iso_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

    if (ss.fail()) {
        throw std::runtime_error("Failed to parse ISO8601 timestamp: " +
                                 iso_string);
    }

    return system_clock::from_time_t(timegm(&tm));
}
