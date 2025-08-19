#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "curl.hpp"
#include "downloader.hpp"
#include "json.hpp"

class CosmosDB {
  public:
    explicit CosmosDB(const nlohmann::json &config);
    ~CosmosDB() = default;

    CosmosDB(const CosmosDB &) = delete;
    CosmosDB &operator=(const CosmosDB &) = delete;
    CosmosDB(CosmosDB &&) = default;
    CosmosDB &operator=(CosmosDB &&) = default;

    std::optional<nlohmann::json> get_item();

  private:
    nlohmann::json config;
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
    std::string url_encode(const std::string &value) const;
};
