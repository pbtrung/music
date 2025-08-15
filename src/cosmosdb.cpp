#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "cosmosdb.hpp"
#include "utils.hpp"

CosmosDB::CosmosDB(const nlohmann::json &config) : config_(config) {
    // Validate required configuration
    if (!config_.contains("cosmos_uri") || !config_["cosmos_uri"].is_string()) {
        throw std::invalid_argument("Missing or invalid cosmos_uri in config");
    }
    if (!config_.contains("cosmos_key") || !config_["cosmos_key"].is_string()) {
        throw std::invalid_argument("Missing or invalid cosmos_key in config");
    }
    if (!config_.contains("cosmos_db_name") ||
        !config_["cosmos_db_name"].is_string()) {
        throw std::invalid_argument(
            "Missing or invalid cosmos_db_name in config");
    }
    if (!config_.contains("cosmos_container") ||
        !config_["cosmos_container"].is_string()) {
        throw std::invalid_argument(
            "Missing or invalid cosmos_container in config");
    }
}

std::string CosmosDB::create_rfc1123_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%a, %d %b %Y %H:%M:%S GMT");
    return ss.str();
}

std::vector<unsigned char>
CosmosDB::decode_base64_key(const std::string &encoded_key) const {
    BIO *bio_mem = BIO_new_mem_buf(encoded_key.c_str(), -1);
    BIO *bio_b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
    bio_mem = BIO_push(bio_b64, bio_mem);

    std::vector<unsigned char> decoded(encoded_key.size());
    int decoded_len =
        BIO_read(bio_mem, decoded.data(), static_cast<int>(decoded.size()));

    BIO_free_all(bio_mem);

    if (decoded_len <= 0) {
        throw std::runtime_error("Failed to decode base64 key");
    }

    decoded.resize(decoded_len);
    return decoded;
}

std::string
CosmosDB::create_hmac_signature(const std::vector<unsigned char> &key,
                                const std::string &message) const {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;

    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(message.c_str()),
              message.length(), mac, &mac_len)) {
        throw std::runtime_error("HMAC computation failed");
    }

    // Base64 encode the MAC
    BIO *bio_mem = BIO_new(BIO_s_mem());
    BIO *bio_b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
    bio_b64 = BIO_push(bio_b64, bio_mem);

    BIO_write(bio_b64, mac, mac_len);
    BIO_flush(bio_b64);

    BUF_MEM *buffer_ptr;
    BIO_get_mem_ptr(bio_b64, &buffer_ptr);

    std::string encoded(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio_b64);

    return encoded;
}

std::string CosmosDB::build_signature_payload(std::string &http_verb,
                                              std::string &resource_type,
                                              std::string &resource_link,
                                              std::string &timestamp) const {
    Utilities::to_lower(http_verb);
    Utilities::to_lower(resource_type);
    Utilities::to_lower(timestamp);
    return http_verb + "\n" + resource_type + "\n" + resource_link + "\n" +
           timestamp + "\n\n";
}

std::string CosmosDB::create_auth_token(std::string &http_verb,
                                        std::string &resource_type,
                                        std::string &resource_link,
                                        std::string &timestamp) const {
    std::string payload = build_signature_payload(http_verb, resource_type,
                                                  resource_link, timestamp);

    auto decoded_key =
        decode_base64_key(config_["cosmos_key"].get<std::string>());
    std::string signature = create_hmac_signature(decoded_key, payload);

    std::string token_plain = "type=master&ver=1.0&sig=" + signature;

    // URL encode the token
    Curl curl;
    char *encoded = curl_easy_escape(curl.handle(), token_plain.c_str(),
                                     static_cast<int>(token_plain.length()));
    if (!encoded) {
        throw std::runtime_error("Failed to URL encode auth token");
    }

    std::string result(encoded);
    curl_free(encoded);

    return result;
}

std::optional<nlohmann::json>
CosmosDB::fetch_cosmos_item(const std::string &track_id) const {
    std::string timestamp = create_rfc1123_timestamp();

    std::string resource_link =
        "dbs/" + config_["cosmos_db_name"].get<std::string>() + "/colls/" +
        config_["cosmos_container"].get<std::string>() + "/docs/" + track_id;

    std::string http_verb = "GET";
    std::string resource_type = "docs";
    std::string auth_token =
        create_auth_token(http_verb, resource_type, resource_link, timestamp);

    std::string url =
        config_["cosmos_uri"].get<std::string>() + "/" + resource_link;

    Curl curl;

    // Set headers using Curl::set_header
    curl.set_header("Accept: application/json");
    curl.set_header("x-ms-date: " + timestamp);
    curl.set_header("x-ms-version: " + cosmos_api_version_);
    curl.set_header("authorization: " + auth_token);
    curl.set_header("x-ms-documentdb-partitionkey: [\"" + track_id + "\"]");

    try {
        curl.set_option(CURLOPT_URL, url);
        curl.set_option(CURLOPT_CUSTOMREQUEST, "GET");
        curl.set_option(CURLOPT_TIMEOUT, 30L);
        curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);

        curl.perform();

        long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);

        if (response_code == 404) {
            return std::nullopt; // Not found
        }

        if (response_code != 200) {
            throw std::runtime_error("HTTP request failed with status: " +
                                     std::to_string(response_code));
        }

        const std::string &response = curl.get_response();
        if (response.empty()) {
            return std::nullopt;
        }

        auto document = nlohmann::json::parse(response);
        return document;

    } catch (const std::exception &e) {
        throw;
    }
}

std::string CosmosDB::generate_random_track_id() const {
    static std::random_device rd;
    static std::mt19937 gen(rd());

    int max_value = config_.value("max_value", 1000000);
    std::uniform_int_distribution<> dis(1, max_value);

    return std::to_string(dis(gen));
}

bool CosmosDB::is_not_found_response(const nlohmann::json &document) const {
    if (!document.contains("code") || !document["code"].is_string()) {
        return false;
    }

    return document["code"].get<std::string>() == "NotFound";
}

std::optional<nlohmann::json> CosmosDB::get_item() {
    int max_retries = config_.value("max_retries", 5);

    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        std::string track_id = generate_random_track_id();

        try {
            auto document = fetch_cosmos_item(track_id);

            if (!document.has_value()) {
                continue; // Not found, try again
            }

            if (is_not_found_response(document.value())) {
                continue; // Not found response, try again
            }

            return document;

        } catch (const std::exception &e) {
            if (attempt == max_retries) {
                throw; // Re-throw on last attempt
            }
            // Continue trying on other attempts
        }
    }

    return std::nullopt;
}

std::optional<FileInfo>
CosmosDB::create_file_info(const nlohmann::json &document) {
    if (document.empty()) {
        return std::nullopt;
    }

    FileInfo info;

    // Extract track_name
    if (document.contains("track_name") && document["track_name"].is_string()) {
        info.track_name = document["track_name"].get<std::string>();
    }

    // Extract album path
    if (document.contains("album") && document["album"].is_object()) {
        const auto &album = document["album"];
        if (album.contains("path") && album["path"].is_string()) {
            info.album_path = album["path"].get<std::string>();
        }
    }

    // Extract track_id
    if (document.contains("track_id")) {
        if (document["track_id"].is_number_integer()) {
            info.track_id = document["track_id"].get<int>();
        } else if (document["track_id"].is_string()) {
            try {
                info.track_id =
                    std::stoi(document["track_id"].get<std::string>());
            } catch (const std::exception &) {
                info.track_id = 0;
            }
        }
    }

    // Generate extension and filename from track_name
    if (!info.track_name.empty()) {
        size_t dot_pos = info.track_name.find_last_of('.');
        if (dot_pos != std::string::npos) {
            info.extension = info.track_name.substr(dot_pos);
        }

        // Generate filename (remove extension and clean up)
        info.filename = info.track_name;
        if (dot_pos != std::string::npos) {
            info.filename = info.filename.substr(0, dot_pos);
        }

        // Replace invalid filename characters
        std::string invalid_chars = "\\/:*?\"<>|";
        for (char c : invalid_chars) {
            std::replace(info.filename.begin(), info.filename.end(), c, '_');
        }
    }

    // Extract CIDs
    if (document.contains("cids") && document["cids"].is_array()) {
        const auto &cids_array = document["cids"];
        info.cids.reserve(cids_array.size());
        info.cid_download_status.reserve(cids_array.size());

        for (const auto &cid_item : cids_array) {
            if (cid_item.is_string()) {
                info.cids.push_back(cid_item.get<std::string>());
                info.cid_download_status.push_back(DownloadStatus::PENDING);
            }
        }
    }

    return info;
}
