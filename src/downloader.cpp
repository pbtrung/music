#include <filesystem>
#include <fstream>

#include <fmt/base.h>
#include <spdlog/spdlog.h>

#include "downloader.hpp"
#include "thread_pool.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;

Downloader::Downloader(const nlohmann::json &config,
                       const nlohmann::json &track)
    : config(config), track(track) {
    cid_download_status.resize(track["cids"].size(), DownloadStatus::PENDING);
    file_download_status = DownloadStatus::PENDING;
}

void Downloader::download_file() {
    dp::ThreadPool thread_pool(config["ncores"].get<int>() *
                               config["mul_factor"].get<int>());
    for (size_t i = 0; i < track["cids"].size(); ++i) {
        auto task = [this, i]() { this->download_cid(i); };
        thread_pool.enqueue_detach(task);
    }
    thread_pool.wait_for_tasks();
}

void Downloader::download_cid(int cid_index) {
    const std::vector<std::string> &gateways = config["gateways"];
    const std::vector<std::string> &cids = track["cids"];
    const int timeout = config["timeout"];
    const int max_retries = config["max_retries"];

    fs::path file_path = fs::path(config["output"]) / cids[cid_index];
    std::ofstream outfile(file_path, std::ios::binary);
    if (!outfile.is_open()) {
        SPDLOG_TRACE("Failed to open file: {}", file_path.string());
        cid_download_status[cid_index] = DownloadStatus::FAILED;
        return;
    }

    Curl curl;
    curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);
    try {
        long response_code = 0;
        int curl_perform = 0;
        std::string url;

        for (int retries = 0; retries < max_retries; ++retries) {
            if (cids[cid_index].size() == 59) {
                url = fmt::format("https://{}.{}", cids[cid_index],
                                  config["n_gateway"].get<std::string>());
                curl.set_option(CURLOPT_TIMEOUT, 2 * timeout);
            } else {
                std::string gateway;
                if (retries == 3 || retries == 4) {
                    gateway = config["i_gateway"].get<std::string>();
                } else {
                    Utilities util;
                    auto random_index =
                        util.generate_unique_ints(1, 0, gateways.size() - 1)
                            .value();
                    gateway = gateways[random_index.front()];
                }
                url = fmt::format("https://{}/{}", gateway, cids[cid_index]);
                curl.set_option(CURLOPT_TIMEOUT, timeout);
            }
            curl.set_option(CURLOPT_URL, url);
            SPDLOG_TRACE("Downloading {} from {}", cids[cid_index], url);

            curl_perform = curl.perform();
            if (curl_perform == CURLE_OK) {
                response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);

                char *content_type_ptr =
                    curl.get_info<char *>(CURLINFO_CONTENT_TYPE);
                std::string content_type =
                    content_type_ptr ? content_type_ptr : "";

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
