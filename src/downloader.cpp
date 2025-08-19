#include <filesystem>
#include <fstream>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "downloader.hpp"
#include "thread_pool.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;

Downloader::Downloader(const nlohmann::json &config,
                       const nlohmann::json &track)
    : config(config), track(track) {
    const auto cid_count = track["cids"].size();
    cid_download_status.resize(cid_count, DownloadStatus::PENDING);
    file_download_status = DownloadStatus::PENDING;
}

void Downloader::download_file() {
    const int thread_count =
        config["ncores"].get<int>() * config["mul_factor"].get<int>();
    dp::ThreadPool thread_pool(thread_count);

    const auto cid_count = track["cids"].size();
    for (size_t i = 0; i < cid_count; ++i) {
        auto task = [this, i]() { this->download_cid(static_cast<int>(i)); };
        thread_pool.enqueue_detach(std::move(task));
    }

    thread_pool.wait_for_tasks();
}

void Downloader::download_cid(int cid_index) {
    const auto &gateways = config["gateways"].get<std::vector<std::string>>();
    const auto &cids = track["cids"].get<std::vector<std::string>>();
    const int timeout = config["timeout"].get<int>();
    const int max_retries = config["max_retries"].get<int>();
    const std::string &current_cid = cids[cid_index];

    // Create output directory if it doesn't exist
    const fs::path output_dir = config["output"].get<std::string>();
    std::error_code ec;
    fs::create_directories(output_dir, ec);

    const fs::path file_path = output_dir / current_cid;
    const fs::path temp_path = fs::path(file_path).concat(".tmp");

    // Use RAII for file management
    std::ofstream outfile(temp_path, std::ios::binary);
    if (!outfile.is_open()) {
        SPDLOG_TRACE("Failed to open file: {}", temp_path.string());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    // Determine if this is a special CID (length 59)
    const bool is_special_cid = current_cid.size() == 59;

    try {
        if (attempt_download(current_cid, outfile, gateways, timeout,
                             max_retries, is_special_cid)) {
            outfile.close();

            // Atomic rename on success
            fs::rename(temp_path, file_path, ec);
            if (ec) {
                SPDLOG_TRACE("Failed to rename {} to {}: {}",
                             temp_path.string(), file_path.string(),
                             ec.message());
                cid_download_status[cid_index] = DownloadStatus::FAILED;
            } else {
                cid_download_status[cid_index] = DownloadStatus::SUCCEEDED;
                SPDLOG_TRACE("Successfully downloaded: {}", current_cid);
            }
        } else {
            cid_download_status[cid_index] = DownloadStatus::FAILED;
        }
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Download error for {}: {}", current_cid, e.what());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
    }

    // Clean up temporary file on failure
    if (cid_download_status[cid_index] == DownloadStatus::FAILED) {
        fs::remove(temp_path, ec); // Ignore errors during cleanup
    }
}

bool Downloader::attempt_download(const std::string &cid,
                                  std::ofstream &outfile,
                                  const std::vector<std::string> &gateways,
                                  int timeout, int max_retries,
                                  bool is_special_cid) {

    Curl curl;
    curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
    curl.set_file_output(&outfile);

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        const std::string url =
            build_download_url(cid, gateways, attempt, is_special_cid);
        const int current_timeout = is_special_cid ? (2 * timeout) : timeout;

        curl.set_option(CURLOPT_URL, url);
        curl.set_option(CURLOPT_TIMEOUT, current_timeout);

        SPDLOG_TRACE("Downloading {} from {} (attempt {}/{})", cid, url,
                     attempt + 1, max_retries);

        const int curl_result = curl.perform();

        if (curl_result == CURLE_OK) {
            const long response_code =
                curl.get_info<long>(CURLINFO_RESPONSE_CODE);

            if (response_code == 200 &&
                is_valid_response(curl, is_special_cid)) {
                SPDLOG_TRACE("Download completed: {}", cid);
                return true;
            }

            SPDLOG_TRACE("Invalid response for {}: code={}", cid,
                         response_code);
        }

        // Reset file position for retry
        reset_output_file(outfile);
    }

    spdlog::warn("Download failed for {} after {} attempts", cid, max_retries);
    return false;
}

std::string
Downloader::build_download_url(const std::string &cid,
                               const std::vector<std::string> &gateways,
                               int attempt, bool is_special_cid) const {
    if (is_special_cid) {
        const std::string &n_gateway = config["n_gateway"].get<std::string>();
        return fmt::format("https://{}.{}", cid, n_gateway);
    }

    std::string gateway;
    if (attempt == 3 || attempt == 4) {
        gateway = config["i_gateway"].get<std::string>();
    } else {
        gateway = select_random_gateway(gateways);
    }

    return fmt::format("https://{}/{}", gateway, cid);
}

std::string Downloader::select_random_gateway(
    const std::vector<std::string> &gateways) const {
    if (gateways.empty()) {
        throw std::runtime_error("No gateways available");
    }

    static thread_local Utilities util;
    const auto random_indices =
        util.generate_unique_ints(1, 0, gateways.size() - 1);

    if (!random_indices || random_indices->empty()) {
        // Fallback to first gateway if random selection fails
        spdlog::warn("Random gateway selection failed, using first gateway");
        return gateways.front();
    }

    return gateways[random_indices->front()];
}

bool Downloader::is_valid_response(const Curl &curl,
                                   bool is_special_cid) const {
    if (is_special_cid) {
        return true; // Special CIDs don't need content-type validation
    }

    const char *content_type_ptr = curl.get_info<char *>(CURLINFO_CONTENT_TYPE);
    const std::string content_type = content_type_ptr ? content_type_ptr : "";

    return !content_type.empty() && content_type == "application/octet-stream";
}

void Downloader::reset_output_file(std::ofstream &outfile) const {
    outfile.clear();
    outfile.seekp(0, std::ios::beg);
}
