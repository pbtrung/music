#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "downloader.hpp"
#include "thread_pool.hpp"
#include "utils.hpp"

using namespace std::chrono;
using json = nlohmann::json;
namespace fs = std::filesystem;

Downloader::Downloader(const nlohmann::json &config,
                       const nlohmann::json &track)
    : config(config), track(track), completed_cids(0) {
    cid_download_status.resize(track["cids"].size(), DownloadStatus::PENDING);
}

void Downloader::download_file() {
    const int thread_count =
        config["ncores"].get<int>() * config["mul_factor"].get<int>();
    dp::ThreadPool thread_pool(thread_count);

    for (size_t i = 0; i < track["cids"].size(); ++i) {
        thread_pool.enqueue_detach(
            [this, i]() { download_single_cid(static_cast<int>(i)); });
    }

    thread_pool.wait_for_tasks();
}

bool Downloader::succeeded() const {
    for (const auto &status : cid_download_status) {
        if (status != DownloadStatus::SUCCEEDED)
            return false;
    }
    return true;
}

std::optional<std::string> Downloader::assemble_file() {
    const auto &cids = track["cids"].get<std::vector<std::string>>();

    if (cids.size() == 1) {
        return handle_single_file();
    }

    return assemble_multiple_files();
}

// Main download workflow
void Downloader::download_single_cid(int cid_index) {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const std::string &cid = cids[cid_index];

    ensure_output_directory();
    const auto temp_path = get_temp_path(cid);

    std::ofstream outfile(temp_path, std::ios::binary);
    if (!outfile.is_open()) {
        SPDLOG_TRACE("Failed to open temp file: {}", temp_path.string());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    const bool success = execute_download(cid, outfile);
    outfile.close();

    finalize_download(cid_index, cid, temp_path, success);
}

bool Downloader::execute_download(const std::string &cid,
                                  std::ofstream &outfile) {
    const auto cid_type = get_cid_type(cid);

    switch (cid_type) {
    case CidType::GDR:
        return download_via_gdr(cid, outfile);
    case CidType::IPFS:
        return download_via_ipfs(cid, outfile, true);
    case CidType::ARW:
    default:
        return download_via_ipfs(cid, outfile, false);
    }
}

void Downloader::finalize_download(int cid_index, const std::string &cid,
                                   const fs::path &temp_path, bool success) {
    if (!success) {
        cleanup_temp_file(temp_path);
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    const auto cid_type = get_cid_type(cid);
    const auto final_path = get_final_path(cid, cid_type);

    std::error_code ec;
    fs::rename(temp_path, final_path, ec);

    if (ec) {
        SPDLOG_TRACE("Failed to rename {} to {}: {}", temp_path.string(),
                     final_path.string(), ec.message());
        cleanup_temp_file(temp_path);
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    cid_download_status[cid_index] = DownloadStatus::SUCCEEDED;
    log_download_progress(cid_index, cid);
}

// CID type detection
CidType Downloader::get_cid_type(const std::string &cid) const {
    if (cid.size() == 45)
        return CidType::GDR;
    if (cid.size() == 59)
        return CidType::IPFS;
    return CidType::ARW;
}

// IPFS download methods
bool Downloader::download_via_ipfs(const std::string &cid,
                                   std::ofstream &outfile, bool is_special) {
    const int max_retries = config["max_retries"].get<int>();

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        const auto url = build_ipfs_url(cid, attempt, is_special);
        const int timeout = is_special ? (2 * config["timeout"].get<int>())
                                       : config["timeout"].get<int>();

        if (try_ipfs_download(cid, outfile, url, timeout)) {
            return true;
        }

        reset_file_position(outfile);
    }

    return false;
}

bool Downloader::try_ipfs_download(const std::string &cid,
                                   std::ofstream &outfile,
                                   const std::string &url, int timeout) {
    Curl curl;
    curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
    curl.set_option(CURLOPT_URL, url);
    curl.set_option(CURLOPT_TIMEOUT, timeout);
    curl.set_file_output(&outfile);

    const int result = curl.perform();
    if (result != CURLE_OK)
        return false;

    const long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
    if (response_code != 200)
        return false;

    const bool is_special = get_cid_type(cid) == CidType::IPFS;
    return validate_ipfs_response(curl, is_special);
}

std::string Downloader::build_ipfs_url(const std::string &cid, int attempt,
                                       bool is_special) const {
    if (is_special) {
        return fmt::format("https://{}.{}", cid,
                           config["n_gateway"].get<std::string>());
    }

    const std::string gateway = get_gateway(attempt);
    return fmt::format("https://{}/{}", gateway, cid);
}

std::string Downloader::get_gateway(int attempt) const {
    if (attempt == 3 || attempt == 4) {
        return config["i_gateway"].get<std::string>();
    }

    const auto &gateways = config["gateways"].get<std::vector<std::string>>();
    const auto random_indices =
        Utilities::generate_unique_ints(1, 0, gateways.size() - 1);

    if (!random_indices || random_indices->empty()) {
        return gateways.front();
    }

    return gateways[random_indices->front()];
}

bool Downloader::validate_ipfs_response(const Curl &curl,
                                        bool is_special) const {
    if (is_special)
        return true;

    const char *content_type_ptr = curl.get_info<char *>(CURLINFO_CONTENT_TYPE);
    const std::string content_type = content_type_ptr ? content_type_ptr : "";

    return content_type == "application/octet-stream";
}

// Google Drive download methods
bool Downloader::download_via_gdr(const std::string &file_id,
                                  std::ofstream &outfile) {
    const int max_retries = config["max_retries"].get<int>();
    const int timeout = config["timeout"].get<int>();

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        const auto token = get_fresh_token();

        Curl curl;
        curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
        curl.set_option(CURLOPT_USERAGENT, "GDrDownloader/1.0");
        curl.set_option(CURLOPT_TIMEOUT, timeout);
        curl.set_file_output(&outfile);

        const auto url = fmt::format(
            "https://www.googleapis.com/drive/v3/files/{}?alt=media", file_id);
        curl.set_option(CURLOPT_URL, url);
        curl.set_header(fmt::format("Authorization: Bearer {}", token));
        curl.set_header(fmt::format("Range: bytes={}-{}",
                                    track["byte_range"][0].get<int>(),
                                    track["byte_range"][1].get<int>()));

        const int result = curl.perform();
        if (result != CURLE_OK) {
            reset_file_position(outfile);
            continue;
        }

        const long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
        if (response_code == 200 || response_code == 206) {
            return true;
        }

        reset_file_position(outfile);
        std::this_thread::sleep_for(milliseconds(1000 * (1 << attempt)));
    }

    return false;
}

std::string Downloader::get_fresh_token() {
    if (is_token_valid()) {
        return config["access_token"].get<std::string>();
    }

    return request_new_token();
}

std::string Downloader::request_new_token() {
    const int max_retries = config["max_retries"].get<int>();
    const std::string url = "https://oauth2.googleapis.com/token";
    const std::string data = fmt::format(
        "client_id={}&client_secret={}&refresh_token={}&grant_type=refresh_token",
        config["client_id"].get<std::string>(),
        config["client_secret"].get<std::string>(),
        config["refresh_token"].get<std::string>());

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        Curl curl;
        curl.reset_string_output();
        curl.set_option(CURLOPT_URL, url);
        curl.set_option(CURLOPT_POSTFIELDS, data);
        curl.set_option(CURLOPT_TIMEOUT, config["timeout"].get<int>());
        curl.set_header("Content-Type: application/x-www-form-urlencoded");

        const int result = curl.perform();
        if (result != CURLE_OK) {
            SPDLOG_TRACE("Token refresh attempt {} failed with curl error: {}",
                         attempt + 1, result);
            if (attempt < max_retries - 1) {
                std::this_thread::sleep_for(
                    milliseconds(1000 * (1 << attempt)));
                continue;
            }
            throw std::runtime_error("Token refresh request failed after " +
                                     std::to_string(max_retries) + " attempts");
        }

        const auto response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
        if (response_code != 200) {
            SPDLOG_TRACE("Token refresh attempt {} failed with HTTP error: {}",
                         attempt + 1, response_code);
            if (attempt < max_retries - 1) {
                std::this_thread::sleep_for(
                    milliseconds(1000 * (1 << attempt)));
                continue;
            }
            throw std::runtime_error(
                "Token refresh HTTP error: " + std::to_string(response_code) +
                " after " + std::to_string(max_retries) + " attempts");
        }

        // Success - parse and return the token
        try {
            const auto token_data = json::parse(curl.get_response());
            const std::string access_token = token_data["access_token"];
            const int expires_in = token_data.value("expires_in", 3600);
            const auto expiry = system_clock::now() + seconds(expires_in - 30);

            config["access_token"] = access_token;
            config["expiry_iso"] = to_iso8601(expiry);

            SPDLOG_TRACE("Token refresh succeeded on attempt {}", attempt + 1);
            return access_token;
        } catch (const json::exception &e) {
            SPDLOG_TRACE("Token refresh attempt {} failed to parse JSON: {}",
                         attempt + 1, e.what());
            if (attempt < max_retries - 1) {
                std::this_thread::sleep_for(
                    milliseconds(1000 * (1 << attempt)));
                continue;
            }
            throw std::runtime_error(
                "Token refresh JSON parsing failed after " +
                std::to_string(max_retries) + " attempts: " + e.what());
        }
    }

    // This should never be reached due to the throw statements above
    throw std::runtime_error("Token refresh failed unexpectedly");
}

bool Downloader::is_token_valid() const {
    if (!config.contains("access_token") || !config.contains("expiry_iso")) {
        return false;
    }

    try {
        const auto expiry = from_iso8601(config["expiry_iso"]);
        return expiry > system_clock::now() + minutes(1);
    } catch (...) {
        return false;
    }
}

// File management
fs::path Downloader::get_temp_path(const std::string &cid) const {
    const fs::path output_dir = config["output"].get<std::string>();
    return output_dir / (cid + ".tmp");
}

fs::path Downloader::get_final_path(const std::string &cid,
                                    CidType type) const {
    const fs::path output_dir = config["output"].get<std::string>();

    if (type == CidType::GDR || type == CidType::IPFS) {
        const auto filename = generate_output_filename();
        return filename.empty() ? output_dir / cid : output_dir / filename;
    }

    return output_dir / cid;
}

void Downloader::cleanup_temp_file(const fs::path &temp_path) const {
    std::error_code ec;
    fs::remove(temp_path, ec);
}

void Downloader::reset_file_position(std::ofstream &outfile) const {
    outfile.clear();
    outfile.seekp(0, std::ios::beg);
}

// Assembly methods
std::optional<std::string> Downloader::assemble_multiple_files() {
    const auto filename = generate_output_filename();
    if (filename.empty())
        return std::nullopt;

    const fs::path output_dir = config["output"].get<std::string>();
    const fs::path assembled_path = output_dir / filename;

    if (!combine_cid_files(assembled_path))
        return std::nullopt;

    cleanup_cid_files();
    return filename;
}

std::optional<std::string> Downloader::handle_single_file() {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const std::string &cid = cids[0];
    const auto cid_type = get_cid_type(cid);

    if (cid_type == CidType::GDR || cid_type == CidType::IPFS) {
        const auto filename = generate_output_filename();
        return filename.empty() ? std::nullopt : std::make_optional(filename);
    }

    const auto filename = generate_output_filename();
    if (filename.empty())
        return std::nullopt;

    const fs::path output_dir = config["output"].get<std::string>();
    const fs::path source = output_dir / cid;
    const fs::path target = output_dir / filename;

    std::error_code ec;
    fs::rename(source, target, ec);

    return ec ? std::nullopt : std::make_optional(filename);
}

bool Downloader::combine_cid_files(const fs::path &output_path) {
    std::ofstream output(output_path, std::ios::binary);
    if (!output.is_open())
        return false;

    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = config["output"].get<std::string>();

    for (const auto &cid : cids) {
        const fs::path cid_file = output_dir / cid;
        std::ifstream input(cid_file, std::ios::binary);
        if (!input.is_open())
            return false;

        output << input.rdbuf();
        if (output.fail())
            return false;
    }

    return true;
}

void Downloader::cleanup_cid_files() {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = config["output"].get<std::string>();

    for (const auto &cid : cids) {
        std::error_code ec;
        fs::remove(output_dir / cid, ec);
    }
}

// Utilities
std::string Downloader::generate_output_filename() const {
    const std::string original = track["track_name"].get<std::string>();
    const auto generated = Utilities::generate_filename(original);
    return generated.value_or(original);
}

void Downloader::log_download_progress(int cid_index, const std::string &cid) {
    const int current_completed = completed_cids.fetch_add(1) + 1;
    SPDLOG_TRACE("Downloaded: {} (cid {}/{}, finished {}/{})", cid,
                 cid_index + 1, cid_download_status.size(), current_completed,
                 cid_download_status.size());
}

void Downloader::ensure_output_directory() const {
    const fs::path output_dir = config["output"].get<std::string>();
    std::error_code ec;
    fs::create_directories(output_dir, ec);
}

// Time utilities
std::string Downloader::to_iso8601(const system_clock::time_point &tp) {
    const auto time_t = system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&time_t, &tm);

    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}+00:00",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

system_clock::time_point
Downloader::from_iso8601(const std::string &iso_string) {
    std::tm tm{};
    std::istringstream ss(iso_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");

    if (ss.fail()) {
        throw std::runtime_error("Invalid ISO8601 format: " + iso_string);
    }

    return system_clock::from_time_t(timegm(&tm));
}
