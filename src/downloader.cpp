#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <fmt/base.h>
#include <fmt/format.h>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include "downloader.hpp"
#include "thread_pool.hpp"
#include "utils.hpp"

using namespace std::chrono;
using json = nlohmann::json;
namespace fs = std::filesystem;

// =============================================================================
// CidUtils Implementation
// =============================================================================

namespace CidUtils {
std::string to_string(CidType type) {
    switch (type) {
    case CidType::GDR:
        return "GDR";
    case CidType::IPFS:
        return "IPFS";
    case CidType::ARW:
        return "ARW";
    default:
        return "UNKNOWN";
    }
}

CidType detect_type(const std::string &cid, bool has_byte_range) {
    if (has_byte_range) {
        return CidType::GDR;
    }
    return (cid.size() == 43) ? CidType::ARW : CidType::IPFS;
}
} // namespace CidUtils

// =============================================================================
// BaseDownloader Implementation
// =============================================================================

BaseDownloader::BaseDownloader(nlohmann::json &config,
                               const nlohmann::json &track)
    : config(config), track(track) {
    SPDLOG_TRACE("BaseDownloader initialized for track: '{}'",
                 track["track_name"].get<std::string>());
}

void BaseDownloader::reset_file_position(std::ofstream &outfile) const {
    SPDLOG_TRACE("Resetting file position to beginning");
    outfile.clear();
    outfile.seekp(0, std::ios::beg);
}

std::string BaseDownloader::to_iso8601(const system_clock::time_point &tp) {
    const auto result = std::format("{:%Y-%m-%dT%H:%M:%S}", tp);
    SPDLOG_TRACE("Converted time_point to ISO8601: {}", result);
    return result;
}

system_clock::time_point
BaseDownloader::from_iso8601(const std::string &iso_string) {
    SPDLOG_TRACE("Parsing ISO8601 string: {}", iso_string);

    system_clock::time_point tp;
    std::istringstream ss(iso_string);
    ss >> std::chrono::parse("%Y-%m-%dT%H:%M:%S", tp);

    if (ss.fail()) {
        SPDLOG_TRACE("Failed to parse ISO8601 string: {}", iso_string);
        throw std::runtime_error("Invalid ISO8601 format: " + iso_string);
    }

    SPDLOG_TRACE("Successfully parsed ISO8601 string: {}", iso_string);
    return tp;
}

// =============================================================================
// IPFSDownloader Implementation
// =============================================================================

IPFSDownloader::IPFSDownloader(nlohmann::json &config,
                               const nlohmann::json &track)
    : BaseDownloader(config, track) {
    SPDLOG_TRACE("IPFSDownloader initialized");
}

bool IPFSDownloader::download(const std::string &cid, std::ofstream &outfile) {
    const int max_retries = config["max_retries"].get<int>();

    SPDLOG_TRACE("Starting IPFS download for CID '{}': max_retries={}", cid,
                 max_retries);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        const auto url = build_url(cid);
        const int timeout =
            2 * config["timeout"].get<int>(); // Special timeout for IPFS

        SPDLOG_TRACE(
            "IPFS download attempt {}/{} for CID '{}': URL='{}', timeout={}s",
            attempt + 1, max_retries, cid, url, timeout);

        if (try_download_attempt(cid, outfile, url, timeout)) {
            SPDLOG_TRACE(
                "IPFS download succeeded for CID '{}' on attempt {}/{}", cid,
                attempt + 1, max_retries);
            return true;
        }

        SPDLOG_TRACE(
            "IPFS download attempt {}/{} failed for CID '{}', resetting file position",
            attempt + 1, max_retries, cid);
        reset_file_position(outfile);
    }

    SPDLOG_TRACE("IPFS download failed for CID '{}' after {} attempts", cid,
                 max_retries);
    return false;
}

bool IPFSDownloader::try_download_attempt(const std::string &cid,
                                          std::ofstream &outfile,
                                          const std::string &url, int timeout) {
    SPDLOG_TRACE("Attempting IPFS download for CID '{}' from URL: {}", cid,
                 url);

    Curl curl;
    curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
    curl.set_option(CURLOPT_URL, url);
    curl.set_option(CURLOPT_TIMEOUT, timeout);
    curl.set_file_output(&outfile);

    const int result = curl.perform();
    if (result != CURLE_OK) {
        SPDLOG_TRACE("CURL perform failed for CID '{}': error_code={}", cid,
                     result);
        return false;
    }

    const long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
    if (response_code != 200) {
        SPDLOG_TRACE("HTTP error for CID '{}': response_code={}", cid,
                     response_code);
        return false;
    }

    const bool validation_result = validate_response(curl);
    SPDLOG_TRACE("IPFS download validation for CID '{}': validation_passed={}",
                 cid, validation_result);

    return validation_result;
}

std::string IPFSDownloader::build_url(const std::string &cid) const {
    const std::string url = fmt::format("https://{}.{}", cid,
                                        config["n_gateway"].get<std::string>());
    SPDLOG_TRACE("Built IPFS URL for CID '{}': {}", cid, url);
    return url;
}

bool IPFSDownloader::validate_response(const Curl &curl) const {
    SPDLOG_TRACE("Skipping content type validation for IPFS download");
    return true; // IPFS doesn't require content type validation
}

// =============================================================================
// ARWDownloader Implementation
// =============================================================================

ARWDownloader::ARWDownloader(nlohmann::json &config,
                             const nlohmann::json &track)
    : BaseDownloader(config, track) {
    SPDLOG_TRACE("ARWDownloader initialized");
}

bool ARWDownloader::download(const std::string &cid, std::ofstream &outfile) {
    const int max_retries = config["max_retries"].get<int>();

    SPDLOG_TRACE("Starting ARW download for CID '{}': max_retries={}", cid,
                 max_retries);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        const auto url = build_url(cid, attempt);
        const int timeout = config["timeout"].get<int>();

        SPDLOG_TRACE(
            "ARW download attempt {}/{} for CID '{}': URL='{}', timeout={}s",
            attempt + 1, max_retries, cid, url, timeout);

        if (try_download_attempt(cid, outfile, url, timeout)) {
            SPDLOG_TRACE("ARW download succeeded for CID '{}' on attempt {}/{}",
                         cid, attempt + 1, max_retries);
            return true;
        }

        SPDLOG_TRACE(
            "ARW download attempt {}/{} failed for CID '{}', resetting file position",
            attempt + 1, max_retries, cid);
        reset_file_position(outfile);
    }

    SPDLOG_TRACE("ARW download failed for CID '{}' after {} attempts", cid,
                 max_retries);
    return false;
}

bool ARWDownloader::try_download_attempt(const std::string &cid,
                                         std::ofstream &outfile,
                                         const std::string &url, int timeout) {
    SPDLOG_TRACE("Attempting ARW download for CID '{}' from URL: {}", cid, url);

    Curl curl;
    curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
    curl.set_option(CURLOPT_URL, url);
    curl.set_option(CURLOPT_TIMEOUT, timeout);
    curl.set_file_output(&outfile);

    const int result = curl.perform();
    if (result != CURLE_OK) {
        SPDLOG_TRACE("CURL perform failed for CID '{}': error_code={}", cid,
                     result);
        return false;
    }

    const long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
    if (response_code != 200) {
        SPDLOG_TRACE("HTTP error for CID '{}': response_code={}", cid,
                     response_code);
        return false;
    }

    // Close file to flush data before validation
    outfile.close();

    // Get temp file path for validation
    const fs::path temp_path =
        fs::path(config["output"].get<std::string>()) / (cid + ".tmp");

    const bool validation_result = validate_response_with_path(curl, temp_path);

    // Reopen file for potential next attempt (truncate if validation failed)
    if (validation_result) {
        outfile.open(temp_path, std::ios::binary | std::ios::app);
    } else {
        outfile.open(temp_path, std::ios::binary | std::ios::trunc);
    }

    SPDLOG_TRACE("ARW download validation for CID '{}': validation_passed={}",
                 cid, validation_result);

    return validation_result;
}

std::string ARWDownloader::build_url(const std::string &cid,
                                     int attempt) const {
    const std::string gateway = get_gateway(attempt);
    const std::string url = fmt::format("https://{}/{}", gateway, cid);
    SPDLOG_TRACE("Built ARW URL for CID '{}' (attempt {}): {} via gateway '{}'",
                 cid, attempt + 1, url, gateway);
    return url;
}

std::string ARWDownloader::get_gateway(int attempt) const {
    SPDLOG_TRACE("Selecting gateway for attempt {}", attempt + 1);

    if (attempt == 3 || attempt == 4) {
        const std::string gateway = config["i_gateway"].get<std::string>();
        SPDLOG_TRACE("Using configured i_gateway for attempt {}: {}",
                     attempt + 1, gateway);
        return gateway;
    }

    const auto &gateways = config["gateways"].get<std::vector<std::string>>();
    const auto random_indices =
        Utilities::generate_unique_ints(1, 0, gateways.size() - 1);

    if (!random_indices || random_indices->empty()) {
        const std::string gateway = gateways.front();
        SPDLOG_TRACE("Using first gateway (fallback) for attempt {}: {}",
                     attempt + 1, gateway);
        return gateway;
    }

    const std::string gateway = gateways[random_indices->front()];
    SPDLOG_TRACE(
        "Using randomly selected gateway for attempt {}: {} (index {})",
        attempt + 1, gateway, random_indices->front());
    return gateway;
}

bool ARWDownloader::validate_response_with_path(
    const Curl &curl, const fs::path &file_path) const {
    // Calculate SHA256 of current download
    const std::string current_sha256 = calculate_sha256_from_file(file_path);

    if (current_sha256.empty()) {
        SPDLOG_TRACE(
            "ARW response validation failed: could not calculate SHA256");
        return false;
    }

    SPDLOG_TRACE("ARW response validation: current_sha256='{}'",
                 current_sha256);

    const char *content_type_ptr = curl.get_info<char *>(CURLINFO_CONTENT_TYPE);
    const std::string content_type = content_type_ptr ? content_type_ptr : "";

    // Only cache SHA256s when content type is application/octet-stream
    if (content_type == "application/octet-stream") {
        SPDLOG_TRACE(
            "ARW valid content type detected, checking SHA256 cache: '{}'",
            current_sha256);

        // Check if this SHA256 already exists in cache
        if (sha256_cache.find(current_sha256) != sha256_cache.end()) {
            SPDLOG_TRACE("ARW hash match found: sha256='{}', validation=true",
                         current_sha256);
            return true;
        }

        // Store this SHA256 in cache
        sha256_cache.insert(current_sha256);

        SPDLOG_TRACE("ARW SHA256 cached: sha256='{}', cache_size={}",
                     current_sha256, sha256_cache.size());
    } else {
        SPDLOG_TRACE("ARW invalid content type: '{}', not caching SHA256",
                     content_type);
    }

    // Return false since we need at least two matching SHA256s
    SPDLOG_TRACE(
        "ARW no hash match found yet: sha256='{}', cache_size={}, validation=false",
        current_sha256, sha256_cache.size());

    return false;
}

std::string
ARWDownloader::calculate_sha256_from_file(const fs::path &file_path) const {
    SPDLOG_TRACE("Calculating SHA256 hash for file: {}", file_path.string());

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        SPDLOG_TRACE("Failed to open file for SHA256 calculation: {}",
                     file_path.string());
        return "";
    }

    // Initialize OpenSSL SHA256 context
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        SPDLOG_TRACE("Failed to create EVP_MD_CTX for SHA256");
        return "";
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        SPDLOG_TRACE("Failed to initialize SHA256 digest");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    // Read file in chunks and update hash
    constexpr size_t buffer_size = 8192;
    std::vector<unsigned char> buffer(buffer_size);

    while (file.good() && !file.eof()) {
        file.read(reinterpret_cast<char *>(buffer.data()), buffer_size);
        const std::streamsize bytes_read = file.gcount();

        if (bytes_read > 0) {
            if (EVP_DigestUpdate(ctx, buffer.data(), bytes_read) != 1) {
                SPDLOG_TRACE("Failed to update SHA256 digest");
                EVP_MD_CTX_free(ctx);
                return "";
            }
        }
    }

    // Finalize hash calculation
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        SPDLOG_TRACE("Failed to finalize SHA256 digest");
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);

    // Convert hash to hex string
    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; ++i) {
        hex_stream << std::setw(2) << static_cast<unsigned int>(hash[i]);
    }

    const std::string result = hex_stream.str();

    SPDLOG_TRACE("SHA256 calculation completed: hash='{}', length={}", result,
                 hash_len);
    return result;
}

// =============================================================================
// GDRDownloader Implementation
// =============================================================================

GDRDownloader::GDRDownloader(nlohmann::json &config,
                             const nlohmann::json &track)
    : BaseDownloader(config, track) {
    SPDLOG_TRACE("GDRDownloader initialized");
}

bool GDRDownloader::download(const std::string &file_id,
                             std::ofstream &outfile) {
    const int max_retries = config["max_retries"].get<int>();
    const int timeout = config["timeout"].get<int>();
    const auto &byte_range = track["byte_range"].get<std::vector<int>>();

    SPDLOG_TRACE(
        "Starting Google Drive download for file_id '{}': max_retries={}, "
        "timeout={}s, byte_range=[{}-{}]",
        file_id, max_retries, timeout, byte_range[0], byte_range[1]);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        SPDLOG_TRACE("Google Drive download attempt {}/{} for file_id '{}'",
                     attempt + 1, max_retries, file_id);

        const auto token = get_fresh_token();
        SPDLOG_TRACE("Using access token for file_id '{}': token_length={}",
                     file_id, token.length());

        Curl curl;
        curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
        curl.set_option(CURLOPT_USERAGENT, "GDrDownloader/1.0");
        curl.set_option(CURLOPT_TIMEOUT, timeout);
        curl.set_file_output(&outfile);

        const auto url = fmt::format(
            "https://www.googleapis.com/drive/v3/files/{}?alt=media", file_id);
        curl.set_option(CURLOPT_URL, url);
        curl.set_header(fmt::format("Authorization: Bearer {}", token));
        curl.set_header(
            fmt::format("Range: bytes={}-{}", byte_range[0], byte_range[1]));

        SPDLOG_TRACE(
            "Google Drive request for file_id '{}': URL='{}', range='bytes={}-{}'",
            file_id, url, byte_range[0], byte_range[1]);

        const int result = curl.perform();
        if (result != CURLE_OK) {
            SPDLOG_TRACE("Google Drive CURL perform failed for file_id '{}' "
                         "attempt {}/{}: error_code={}",
                         file_id, attempt + 1, max_retries, result);
            reset_file_position(outfile);
            continue;
        }

        const long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
        if (response_code == 200 || response_code == 206) {
            SPDLOG_TRACE("Google Drive download succeeded for file_id '{}' "
                         "on attempt {}/{}: response_code={}",
                         file_id, attempt + 1, max_retries, response_code);
            return true;
        }

        SPDLOG_TRACE("Google Drive HTTP error for file_id '{}' "
                     "attempt {}/{}: response_code={}",
                     file_id, attempt + 1, max_retries, response_code);

        reset_file_position(outfile);
        const int sleep_ms = 1000 * (1 << attempt);
        SPDLOG_TRACE("Sleeping {}ms before retry for file_id '{}'", sleep_ms,
                     file_id);
        std::this_thread::sleep_for(milliseconds(sleep_ms));
    }

    SPDLOG_TRACE(
        "Google Drive download failed for file_id '{}' after {} attempts",
        file_id, max_retries);
    return false;
}

int GDRDownloader::get_account_index_by_email(const std::string &email) const {
    if (!config.contains("gdr_accounts") ||
        !config["gdr_accounts"].is_array()) {
        return -1; // no accounts
    }

    const auto &accounts = config["gdr_accounts"];
    for (size_t i = 0; i < accounts.size(); ++i) {
        if (accounts[i].contains("email") &&
            accounts[i]["email"].get<std::string>() == email) {
            return static_cast<int>(i);
        }
    }
    return -1; // not found
}

std::string GDRDownloader::get_fresh_token() {
    SPDLOG_TRACE("Checking token validity");

    if (is_token_valid()) {
        const int gdr_account_id =
            get_account_index_by_email(track["email"].get<std::string>());
        auto &gdr_account = config["gdr_accounts"][gdr_account_id];

        const std::string token =
            gdr_account["access_token"].get<std::string>();
        SPDLOG_TRACE("Using existing valid token: length={}", token.length());
        return token;
    }

    SPDLOG_TRACE("Token invalid or expired, requesting new token");
    return request_new_token();
}

std::string GDRDownloader::request_new_token() {
    const int max_retries = config["max_retries"].get<int>();
    const int gdr_account_id =
        get_account_index_by_email(track["email"].get<std::string>());
    auto &gdr_account = config["gdr_accounts"][gdr_account_id];

    const std::string &client_id = gdr_account["client_id"].get<std::string>();
    const std::string &client_secret =
        gdr_account["client_secret"].get<std::string>();
    const std::string &refresh_token =
        gdr_account["refresh_token"].get<std::string>();

    SPDLOG_TRACE(
        "Requesting new token: gdr_account_id={}, max_retries={}, client_id_length={}",
        gdr_account_id, max_retries, client_id.length());

    const std::string url = "https://oauth2.googleapis.com/token";
    const std::string data = fmt::format(
        "client_id={}&client_secret={}&refresh_token={}&grant_type=refresh_token",
        client_id, client_secret, refresh_token);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        SPDLOG_TRACE("Token refresh attempt {}/{}", attempt + 1, max_retries);

        Curl curl;
        curl.reset_string_output();
        curl.set_option(CURLOPT_URL, url);
        curl.set_option(CURLOPT_POSTFIELDS, data.c_str());
        curl.set_option(CURLOPT_TIMEOUT, config["timeout"].get<int>());
        curl.set_header("Content-Type: application/x-www-form-urlencoded");

        const int result = curl.perform();
        if (result != CURLE_OK) {
            SPDLOG_TRACE("Token refresh attempt {} failed with curl error: {}",
                         attempt + 1, result);
            if (attempt < max_retries - 1) {
                const int sleep_ms = 1000 * (1 << attempt);
                SPDLOG_TRACE("Sleeping {}ms before token refresh retry",
                             sleep_ms);
                std::this_thread::sleep_for(milliseconds(sleep_ms));
                continue;
            }
            throw std::runtime_error("Token refresh request failed after " +
                                     std::to_string(max_retries) + " attempts");
        }

        const auto response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
        if (response_code != 200) {
            SPDLOG_TRACE("Token refresh attempt {} failed with HTTP error: {}",
                         attempt + 1, response_code);

            const std::string response = curl.get_response();
            SPDLOG_TRACE("Error response: {}", response);

            if (attempt < max_retries - 1) {
                const int sleep_ms = 1000 * (1 << attempt);
                SPDLOG_TRACE("Sleeping {}ms before token refresh retry",
                             sleep_ms);
                std::this_thread::sleep_for(milliseconds(sleep_ms));
                continue;
            }
            throw std::runtime_error(
                "Token refresh HTTP error: " + std::to_string(response_code) +
                " after " + std::to_string(max_retries) + " attempts");
        }

        // Success - parse and return the token
        try {
            const std::string response = curl.get_response();
            SPDLOG_TRACE("Token refresh response received: length={}",
                         response.length());

            const auto token_data = json::parse(response);
            const std::string access_token = token_data["access_token"];
            const int expires_in = token_data.value("expires_in", 3600);
            const auto expiry = system_clock::now() + seconds(expires_in - 30);

            gdr_account["access_token"] = access_token;
            gdr_account["expiry_iso"] = to_iso8601(expiry);

            SPDLOG_TRACE(
                "Token refresh succeeded on attempt {}: token_length={}, expires_in={}s",
                attempt + 1, access_token.length(), expires_in);
            return access_token;
        } catch (const json::exception &e) {
            SPDLOG_TRACE("Token refresh attempt {} failed to parse JSON: {}",
                         attempt + 1, e.what());
            if (attempt < max_retries - 1) {
                const int sleep_ms = 1000 * (1 << attempt);
                SPDLOG_TRACE("Sleeping {}ms before token refresh retry",
                             sleep_ms);
                std::this_thread::sleep_for(milliseconds(sleep_ms));
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

bool GDRDownloader::is_token_valid() const {
    const int gdr_account_id =
        get_account_index_by_email(track["email"].get<std::string>());
    const auto &gdr_account = config["gdr_accounts"][gdr_account_id];

    if (!gdr_account.contains("access_token") ||
        !gdr_account.contains("expiry_iso")) {
        SPDLOG_TRACE(
            "Token validation failed: missing access_token or expiry_iso");
        return false;
    }

    try {
        const auto expiry = from_iso8601(gdr_account["expiry_iso"]);
        const auto now = system_clock::now();
        const bool is_valid = expiry > now + minutes(1);

        const auto time_diff = duration_cast<seconds>(expiry - now).count();
        SPDLOG_TRACE("Token validation: expires_in={}s, is_valid={}", time_diff,
                     is_valid);

        return is_valid;
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Token validation failed with exception: {}", e.what());
        return false;
    }
}

// =============================================================================
// Main Downloader Implementation
// =============================================================================

Downloader::Downloader(nlohmann::json &config, const nlohmann::json &track)
    : config(config), track(track), completed_cids(0) {
    cid_download_status.resize(track["cids"].size(), DownloadStatus::PENDING);
    SPDLOG_TRACE("Downloader initialized for track: '{}' with {} CIDs",
                 track["track_name"].get<std::string>(), track["cids"].size());
}

void Downloader::download_file() {
    const int thread_count =
        config["ncores"].get<int>() * config["mul_factor"].get<int>();

    SPDLOG_TRACE("Starting download with {} threads for {} CIDs", thread_count,
                 track["cids"].size());

    dp::ThreadPool thread_pool(thread_count);

    for (size_t i = 0; i < track["cids"].size(); ++i) {
        thread_pool.enqueue_detach(
            [this, i]() { download_single_cid(static_cast<int>(i)); });
    }

    SPDLOG_TRACE("All download tasks enqueued, waiting for completion");
    thread_pool.wait_for_tasks();
    SPDLOG_TRACE("All download tasks completed");
}

bool Downloader::succeeded() const {
    const auto failed_count =
        std::count(cid_download_status.begin(), cid_download_status.end(),
                   DownloadStatus::FAILED);
    const auto success_count = cid_download_status.size() - failed_count;
    const bool all_succeeded = (failed_count == 0);

    SPDLOG_TRACE(
        "Download status check: {}/{} succeeded, {}/{} failed, overall success: {}",
        success_count, cid_download_status.size(), failed_count,
        cid_download_status.size(), all_succeeded);

    return all_succeeded;
}

std::optional<std::string> Downloader::assemble_file() {
    const auto &cids = track["cids"].get<std::vector<std::string>>();

    SPDLOG_TRACE("Starting file assembly for {} CIDs", cids.size());

    if (cids.size() == 1) {
        SPDLOG_TRACE("Single file detected, handling single file assembly");
        return handle_single_file();
    }

    SPDLOG_TRACE("Multiple files detected, handling multi-file assembly");
    return assemble_multiple_files();
}

// Main download workflow
void Downloader::download_single_cid(int cid_index) {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const std::string &cid = cids[cid_index];
    const auto cid_type =
        CidUtils::detect_type(cid, track.contains("byte_range"));

    SPDLOG_TRACE("Starting download for CID {}/{}: '{}' (type: {})",
                 cid_index + 1, cids.size(), cid,
                 CidUtils::to_string(cid_type));

    ensure_output_directory();
    const auto temp_path = get_temp_path(cid);

    SPDLOG_TRACE("Opening temp file for CID '{}': {}", cid, temp_path.string());

    std::ofstream outfile(temp_path, std::ios::binary);
    if (!outfile.is_open()) {
        SPDLOG_TRACE("Failed to open temp file for CID '{}': {}", cid,
                     temp_path.string());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    SPDLOG_TRACE("Executing download for CID '{}' (type: {})", cid,
                 CidUtils::to_string(cid_type));
    const bool success = execute_download(cid, outfile);
    outfile.close();

    SPDLOG_TRACE("Download execution completed for CID '{}': success={}", cid,
                 success);

    finalize_download(cid_index, cid, temp_path, success);
}

bool Downloader::execute_download(const std::string &cid,
                                  std::ofstream &outfile) {
    const auto cid_type =
        CidUtils::detect_type(cid, track.contains("byte_range"));

    SPDLOG_TRACE("Executing download for CID '{}' using {} method", cid,
                 CidUtils::to_string(cid_type));

    auto downloader = get_downloader(cid_type);
    if (!downloader) {
        SPDLOG_TRACE("Failed to create downloader for CID '{}' type {}", cid,
                     CidUtils::to_string(cid_type));
        return false;
    }

    const bool result = downloader->download(cid, outfile);
    SPDLOG_TRACE("Download method completed for CID '{}': success={}", cid,
                 result);
    return result;
}

void Downloader::finalize_download(int cid_index, const std::string &cid,
                                   const fs::path &temp_path, bool success) {
    SPDLOG_TRACE("Finalizing download for CID '{}' (index {}): success={}", cid,
                 cid_index, success);

    if (!success) {
        SPDLOG_TRACE("Download failed for CID '{}', cleaning up temp file: {}",
                     cid, temp_path.string());
        cleanup_temp_file(temp_path);
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    const auto cid_type =
        CidUtils::detect_type(cid, track.contains("byte_range"));
    const auto final_path = get_final_path(cid, cid_type);

    SPDLOG_TRACE("Moving temp file for CID '{}' from '{}' to '{}'", cid,
                 temp_path.string(), final_path.string());

    std::error_code ec;
    fs::rename(temp_path, final_path, ec);

    if (ec) {
        SPDLOG_TRACE("Failed to rename '{}' to '{}' for CID '{}': {}",
                     temp_path.string(), final_path.string(), cid,
                     ec.message());
        cleanup_temp_file(temp_path);
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    SPDLOG_TRACE("Successfully finalized download for CID '{}'", cid);
    cid_download_status[cid_index] = DownloadStatus::SUCCEEDED;
    log_download_progress(cid_index, cid);
}

// Downloader creation and management
std::unique_ptr<BaseDownloader> Downloader::get_downloader(CidType type) {
    switch (type) {
    case CidType::IPFS:
        if (!ipfs_downloader) {
            ipfs_downloader = std::make_unique<IPFSDownloader>(config, track);
        }
        return std::make_unique<IPFSDownloader>(config, track);
    case CidType::ARW:
        if (!arw_downloader) {
            arw_downloader = std::make_unique<ARWDownloader>(config, track);
        }
        return std::make_unique<ARWDownloader>(config, track);
    case CidType::GDR:
        if (!gdr_downloader) {
            gdr_downloader = std::make_unique<GDRDownloader>(config, track);
        }
        return std::make_unique<GDRDownloader>(config, track);
    default:
        SPDLOG_TRACE("Unknown CID type: {}", static_cast<int>(type));
        return nullptr;
    }
}

// File management
fs::path Downloader::get_temp_path(const std::string &cid) const {
    const fs::path output_dir = config["output"].get<std::string>();
    const auto temp_path = output_dir / (cid + ".tmp");
    SPDLOG_TRACE("Generated temp path for CID '{}': {}", cid,
                 temp_path.string());
    return temp_path;
}

fs::path Downloader::get_final_path(const std::string &cid,
                                    CidType type) const {
    const fs::path output_dir = config["output"].get<std::string>();
    fs::path final_path = output_dir / cid;
    SPDLOG_TRACE("Generated final path for CID '{}' (type {}): {}", cid,
                 CidUtils::to_string(type), final_path.string());
    return final_path;
}

void Downloader::cleanup_temp_file(const fs::path &temp_path) const {
    SPDLOG_TRACE("Cleaning up temp file: {}", temp_path.string());

    std::error_code ec;
    fs::remove(temp_path, ec);

    if (ec) {
        SPDLOG_TRACE("Failed to cleanup temp file '{}': {}", temp_path.string(),
                     ec.message());
    } else {
        SPDLOG_TRACE("Successfully cleaned up temp file: {}",
                     temp_path.string());
    }
}

// Assembly methods
std::optional<std::string> Downloader::assemble_multiple_files() {
    const auto filename = generate_output_filename();
    if (filename.empty()) {
        SPDLOG_TRACE("Multiple file assembly failed: empty filename generated");
        return std::nullopt;
    }

    const fs::path output_dir = config["output"].get<std::string>();
    const fs::path assembled_path = output_dir / filename;

    SPDLOG_TRACE("Assembling multiple files to: {}", assembled_path.string());

    const bool success = combine_cid_files(assembled_path);
    if (success) {
        SPDLOG_TRACE("Multiple file assembly succeeded");
    } else {
        SPDLOG_TRACE("Multiple file assembly failed");
    }
    cleanup_cid_files();

    return success ? std::make_optional(filename) : std::nullopt;
}

std::optional<std::string> Downloader::handle_single_file() {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const std::string &cid = cids[0];

    const auto filename = generate_output_filename();
    if (filename.empty()) {
        SPDLOG_TRACE(
            "Single file handling failed: empty filename generated for CID '{}'",
            cid);
        return std::nullopt;
    }

    const fs::path output_dir = config["output"].get<std::string>();
    const fs::path source = output_dir / cid;
    const fs::path target = output_dir / filename;

    SPDLOG_TRACE("Renaming single file from '{}' to '{}'", source.string(),
                 target.string());

    std::error_code ec;
    fs::rename(source, target, ec);

    if (ec) {
        SPDLOG_TRACE("Single file rename failed: {}", ec.message());
        return std::nullopt;
    }

    SPDLOG_TRACE("Single file handling succeeded: filename='{}'", filename);
    return std::make_optional(filename);
}

bool Downloader::combine_cid_files(const fs::path &output_path) {
    SPDLOG_TRACE("Combining CID files to: {}", output_path.string());

    std::ofstream output(output_path, std::ios::binary);
    if (!output.is_open()) {
        SPDLOG_TRACE("Failed to open output file for combining: {}",
                     output_path.string());
        return false;
    }

    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = config["output"].get<std::string>();

    for (size_t i = 0; i < cids.size(); ++i) {
        const auto &cid = cids[i];
        const fs::path cid_file = output_dir / cid;

        SPDLOG_TRACE("Combining file {}/{}: '{}' from path '{}'", i + 1,
                     cids.size(), cid, cid_file.string());

        std::ifstream input(cid_file, std::ios::binary);
        if (!input.is_open()) {
            SPDLOG_TRACE("Failed to open CID file for combining: {}",
                         cid_file.string());
            return false;
        }

        output << input.rdbuf();
        if (output.fail()) {
            SPDLOG_TRACE("Failed to write CID file '{}' to combined output",
                         cid);
            return false;
        }

        SPDLOG_TRACE("Successfully combined CID file '{}' ({}/{})", cid, i + 1,
                     cids.size());
    }

    SPDLOG_TRACE("Successfully combined all {} CID files", cids.size());
    return true;
}

void Downloader::cleanup_cid_files() {
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const fs::path output_dir = config["output"].get<std::string>();

    SPDLOG_TRACE("Cleaning up {} individual CID files", cids.size());

    for (size_t i = 0; i < cids.size(); ++i) {
        const auto &cid = cids[i];
        const fs::path cid_file = output_dir / cid;

        std::error_code ec;
        fs::remove(cid_file, ec);

        if (ec) {
            SPDLOG_TRACE("Failed to cleanup CID file '{}' ({}/{}): {}", cid,
                         i + 1, cids.size(), ec.message());
        } else {
            SPDLOG_TRACE("Successfully cleaned up CID file '{}' ({}/{})", cid,
                         i + 1, cids.size());
        }
    }

    SPDLOG_TRACE("Completed cleanup of individual CID files");
}

// Utilities
std::string Downloader::generate_output_filename() const {
    const std::string original = track["track_name"].get<std::string>();
    const auto generated = Utilities::generate_filename(original);
    const std::string result = generated.value_or(original);

    SPDLOG_TRACE("Generated output filename: original='{}', generated='{}'",
                 original, result);
    return result;
}

void Downloader::log_download_progress(int cid_index, const std::string &cid) {
    const int current_completed = completed_cids.fetch_add(1) + 1;
    SPDLOG_TRACE("Downloaded: {} (cid {}/{}, finished {}/{})", cid,
                 cid_index + 1, cid_download_status.size(), current_completed,
                 cid_download_status.size());
}

void Downloader::ensure_output_directory() const {
    const fs::path output_dir = config["output"].get<std::string>();

    SPDLOG_TRACE("Ensuring output directory exists: {}", output_dir.string());

    std::error_code ec;
    fs::create_directories(output_dir, ec);

    if (ec) {
        SPDLOG_TRACE("Failed to create output directory '{}': {}",
                     output_dir.string(), ec.message());
    } else {
        SPDLOG_TRACE("Output directory verified/created: {}",
                     output_dir.string());
    }
}
