#pragma once

#include "curl.hpp"
#include "json.hpp"
#include <optional>
#include <string>

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
    bool succeeded() const;
    std::optional<std::string> assemble_file();

  private:
    void download_cid(int cid_index);
    bool is_valid_response(const Curl &curl, bool is_special_cid) const;
    std::string
    select_random_gateway(const std::vector<std::string> &gateways) const;
    std::string build_download_url(const std::string &cid,
                                   const std::vector<std::string> &gateways,
                                   int attempt, bool is_special_cid) const;
    bool attempt_download(const std::string &cid, std::ofstream &outfile,
                          const std::vector<std::string> &gateways, int timeout,
                          int max_retries, bool is_special_cid);
    void reset_output_file(std::ofstream &outfile) const;

    nlohmann::json track;
    nlohmann::json config;

    std::vector<DownloadStatus> cid_download_status;
    DownloadStatus file_download_status;
};