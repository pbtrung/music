#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include "json.hpp"

class GDrDownloader {
  public:
    explicit GDrDownloader(const nlohmann::json &config);

    // Download a specific file range
    void download_file_range(const std::string &file_id, std::size_t start,
                             std::size_t end,
                             const std::filesystem::path &output_path);

    // Download entire file (helper method)
    void download_file(const std::string &file_id,
                       const std::filesystem::path &output_path);

    // Update config (useful for token updates)
    void update_config(const nlohmann::json &new_config);

    // Get current config
    const nlohmann::json &get_config() const noexcept;

  private:
    mutable nlohmann::json config;

    void validate_config() const;

    std::string get_access_token();
    std::string refresh_access_token();
    bool attempt_download(const std::string &file_id, std::size_t start,
                          std::size_t end, std::ofstream &output_file,
                          int max_retries, int timeout);
    void reset_output_file(std::ofstream &output_file) const;

    static std::string
    timestamp_to_iso8601(const std::chrono::system_clock::time_point &tp);
    static std::chrono::system_clock::time_point
    iso8601_to_timestamp(const std::string &iso_string);
};