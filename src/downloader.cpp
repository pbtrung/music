#include <filesystem>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

#include "downloader.hpp"

namespace fs = std::filesystem;

Downloader::Downloader(const nlohmann::json &config,
                       const nlohmann::json &track)
    : config(config), track(track) {
    cid_download_status.resize(track["cids"].size(), DownloadStatus::PENDING);
    file_download_status = DownloadStatus::PENDING;
}

void Downloader::download_cid(int cid_index) {
    fs::path file_path = fs::path(config["output"]) / cids[cid_index];
    std::ofstream outfile(file_path, std::ios::binary);
    if (!outfile.is_open()) {
        SPDLOG_TRACE("Failed to open file: {}", file_path.string());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    Curl curl;
    try {
        long response_code = 0;
        int curl_perform = 0;
        std::string url;
        const std::vector<std::string> &gateways = config["gateways"];
        const std::vector<std::string> &cids = track["cids"];
        const int timeout = config["timeout"];
        const int max_retries = config["max_retries"];

        size_t count = 0;

        for (int retries = 0; retries < max_retries; ++retries) {
            if (cids[cid_index].size() == 59) {
                url = fmt::format("https://{}.{}", cids[cid_index],
                                  config["n_gateway"]);
                curl.set_option(CURLOPT_TIMEOUT, 2 * timeout);
            } else {
                auto randomIndex =
                    Random::uniqueInts(1, 0, gateways.size() - 1)[0];
                url = fmt::format("https://{}/{}", gateways[randomIndex],
                                  cids[cid_index]);
                curl.set_option(CURLOPT_TIMEOUT, timeout);
            }
            curl.set_option(CURLOPT_URL, url);
            SPDLOG_TRACE("Downloading {} from {}", cids[cid_index], url);

            curl_perform = curl.perform();
            if (curl_perform == CURLE_OK) {
                response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
                std::string content_type(
                    curl.get_info<char *>(CURLINFO_CONTENT_TYPE));
                if (response_code == 200 &&
                    (cids[cid_index].size() == 59 ||
                     (!content_type.empty() &&
                      content_type == "application/octet-stream"))) {
                    cid_download_status[cid_index] = DownloadStatus::SUCCEEDED;
                    break;
                }
            }
            outfile.clear();
            outfile.seekp(0, std::ios::beg);
            SPDLOG_TRACE("Redownload {} (attempt {})", cids[cid_index],
                         retries + 1);
        }

        if (curl_perform != CURLE_OK || response_code != 200) {
            SPDLOG_TRACE("Download of {} failed after {} attempts",
                         cids[cid_index], max_retries);
            cid_download_status[cid_index] = DownloadStatus::FAILED;
        } else {
            SPDLOG_TRACE("Finish {}", cids[cid_index]);
        }
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Error: {}", e.what());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
    }
}
