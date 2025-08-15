#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "curl.hpp"
#include "json.hpp"

enum class DownloadStatus { PENDING, IN_PROGRESS, COMPLETED, FAILED };

struct FileInfo {
    std::string track_name;
    std::string album_path;
    std::string extension;
    std::string filename;
    int track_id = 0;
    std::vector<std::string> cids;
    std::vector<DownloadStatus> cid_download_status;
    DownloadStatus file_download_status = DownloadStatus::PENDING;
};

class CosmosDB {
  public:
    explicit CosmosDB(const nlohmann::json &config);
    ~CosmosDB() = default;

    CosmosDB(const CosmosDB &) = delete;
    CosmosDB &operator=(const CosmosDB &) = delete;
    CosmosDB(CosmosDB &&) = default;
    CosmosDB &operator=(CosmosDB &&) = default;

    std::optional<nlohmann::json> get_item();
    std::optional<FileInfo> create_file_info(const nlohmann::json &document);

  private:
    nlohmann::json config_;
    std::string cosmos_api_version_ = "2018-12-31";

    std::string create_rfc1123_timestamp() const;
    std::vector<unsigned char>
    decode_base64_key(const std::string &encoded_key) const;
    std::string create_hmac_signature(const std::vector<unsigned char> &key,
                                      const std::string &message) const;
    std::string build_signature_payload(std::string &http_verb,
                                        std::string &resource_type,
                                        std::string &resource_link,
                                        std::string &timestamp) const;
    std::string create_auth_token(std::string &http_verb,
                                  std::string &resource_type,
                                  std::string &resource_link,
                                  std::string &timestamp) const;

    std::optional<nlohmann::json>
    fetch_cosmos_item(const std::string &track_id) const;
    std::string generate_random_track_id() const;
    bool is_not_found_response(const nlohmann::json &document) const;
};
