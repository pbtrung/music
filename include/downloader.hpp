#pragma once

#include "curl.hpp"
#include "json.hpp"

enum class DownloadStatus { PENDING, SUCCEEDED, FAILED };

class Downloader {
  public:
    explicit Downloader(const nlohmann::json &config,
                        const nlohmann::json &track);
    ~Downloader() = default;

    Downloader(const Downloader &) = delete;
    Downloader &operator=(const Downloader &) = delete;
    Downloader(Downloader &&) = default;
    Downloader &operator=(Downloader &&) = default;

    void download_file();

  private:
    void download_cid(int cid_index);

    nlohmann::json track;
    nlohmann::json config;

    std::vector<DownloadStatus> cid_download_status;
    DownloadStatus file_download_status;
};
