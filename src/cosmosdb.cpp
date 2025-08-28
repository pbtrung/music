#include <algorithm>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <spdlog/spdlog.h>

#include "cosmosdb.hpp"
#include "utils.hpp"

CosmosDB::CosmosDB(const nlohmann::json &config) : config(config) {
    SPDLOG_TRACE("CosmosDB constructor called");

    // Validate required configuration
    SPDLOG_TRACE("Validating cosmos_uri configuration");
    if (!config.contains("cosmos_uri") || !config["cosmos_uri"].is_string()) {
        SPDLOG_TRACE("cosmos_uri validation failed - missing or invalid");
        throw std::invalid_argument("Missing or invalid cosmos_uri in config");
    }

    SPDLOG_TRACE("Validating cosmos_key configuration");
    if (!config.contains("cosmos_key") || !config["cosmos_key"].is_string()) {
        SPDLOG_TRACE("cosmos_key validation failed - missing or invalid");
        throw std::invalid_argument("Missing or invalid cosmos_key in config");
    }

    SPDLOG_TRACE("Validating cosmos_db_name configuration");
    if (!config.contains("cosmos_db_name") ||
        !config["cosmos_db_name"].is_string()) {
        SPDLOG_TRACE("cosmos_db_name validation failed - missing or invalid");
        throw std::invalid_argument(
            "Missing or invalid cosmos_db_name in config");
    }

    SPDLOG_TRACE("Validating cosmos_container configuration");
    if (!config.contains("cosmos_container") ||
        !config["cosmos_container"].is_string()) {
        SPDLOG_TRACE("cosmos_container validation failed - missing or invalid");
        throw std::invalid_argument(
            "Missing or invalid cosmos_container in config");
    }

    SPDLOG_TRACE("CosmosDB constructor completed successfully");
}

std::string CosmosDB::create_rfc1123_timestamp() const {
    SPDLOG_TRACE("Creating RFC1123 timestamp");
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%a, %d %b %Y %H:%M:%S GMT");

    std::string timestamp = ss.str();
    SPDLOG_TRACE("RFC1123 timestamp created: {}", timestamp);
    return timestamp;
}

std::vector<unsigned char>
CosmosDB::decode_base64_key(const std::string &encoded_key) const {
    SPDLOG_TRACE("Decoding base64 key, length: {}", encoded_key.length());

    BIO *bio_mem = BIO_new_mem_buf(encoded_key.c_str(), -1);
    if (!bio_mem) {
        SPDLOG_TRACE("Failed to create BIO memory buffer");
        throw std::runtime_error("Failed to create BIO memory buffer");
    }

    BIO *bio_b64 = BIO_new(BIO_f_base64());
    if (!bio_b64) {
        SPDLOG_TRACE("Failed to create BIO base64 filter");
        BIO_free(bio_mem);
        throw std::runtime_error("Failed to create BIO base64 filter");
    }

    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
    bio_mem = BIO_push(bio_b64, bio_mem);

    std::vector<unsigned char> decoded(encoded_key.size());
    int decoded_len =
        BIO_read(bio_mem, decoded.data(), static_cast<int>(decoded.size()));

    BIO_free_all(bio_mem);

    if (decoded_len <= 0) {
        SPDLOG_TRACE("Base64 decoding failed, decoded_len: {}", decoded_len);
        throw std::runtime_error("Failed to decode base64 key");
    }

    decoded.resize(decoded_len);
    SPDLOG_TRACE("Base64 key decoded successfully, decoded length: {}",
                 decoded_len);
    return decoded;
}

std::string
CosmosDB::create_hmac_signature(const std::vector<unsigned char> &key,
                                const std::string &message) const {
    SPDLOG_TRACE("Creating HMAC signature for message length: {}",
                 message.length());

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;

    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(message.c_str()),
              message.length(), mac, &mac_len)) {
        SPDLOG_TRACE("HMAC computation failed");
        throw std::runtime_error("HMAC computation failed");
    }

    SPDLOG_TRACE("HMAC computed successfully, MAC length: {}", mac_len);

    // Base64 encode the MAC
    BIO *bio_mem = BIO_new(BIO_s_mem());
    if (!bio_mem) {
        SPDLOG_TRACE("Failed to create BIO memory for base64 encoding");
        throw std::runtime_error(
            "Failed to create BIO memory for base64 encoding");
    }

    BIO *bio_b64 = BIO_new(BIO_f_base64());
    if (!bio_b64) {
        SPDLOG_TRACE("Failed to create BIO base64 for encoding");
        BIO_free(bio_mem);
        throw std::runtime_error("Failed to create BIO base64 for encoding");
    }

    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
    bio_b64 = BIO_push(bio_b64, bio_mem);

    BIO_write(bio_b64, mac, mac_len);
    BIO_flush(bio_b64);

    BUF_MEM *buffer_ptr;
    BIO_get_mem_ptr(bio_b64, &buffer_ptr);

    std::string encoded(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio_b64);

    SPDLOG_TRACE("HMAC signature created successfully, encoded length: {}",
                 encoded.length());
    return encoded;
}

std::string CosmosDB::build_signature_payload(std::string &http_verb,
                                              std::string &resource_type,
                                              std::string &resource_link,
                                              std::string &timestamp) const {
    SPDLOG_TRACE("Building signature payload - verb: {}, type: {}, link: {}",
                 http_verb, resource_type, resource_link);

    Utilities::to_lower(http_verb);
    Utilities::to_lower(resource_type);
    Utilities::to_lower(timestamp);

    std::string payload = http_verb + "\n" + resource_type + "\n" +
                          resource_link + "\n" + timestamp + "\n\n";

    SPDLOG_TRACE("Signature payload built, length: {}", payload.length());
    return payload;
}

std::string CosmosDB::create_auth_token(std::string &http_verb,
                                        std::string &resource_type,
                                        std::string &resource_link,
                                        std::string &timestamp) const {
    SPDLOG_TRACE("Creating auth token for {} request", http_verb);

    std::string payload = build_signature_payload(http_verb, resource_type,
                                                  resource_link, timestamp);

    auto decoded_key =
        decode_base64_key(config["cosmos_key"].get<std::string>());
    std::string signature = create_hmac_signature(decoded_key, payload);

    std::string token_plain = "type=master&ver=1.0&sig=" + signature;
    SPDLOG_TRACE("Plain auth token created, length: {}", token_plain.length());

    std::string encoded_token = url_encode(token_plain);
    SPDLOG_TRACE("Auth token created and URL encoded, final length: {}",
                 encoded_token.length());
    return encoded_token;
}

std::optional<nlohmann::json>
CosmosDB::fetch_cosmos_item(const std::string &track_id) const {
    SPDLOG_TRACE("Fetching Cosmos item with track_id: {}", track_id);

    std::string timestamp = create_rfc1123_timestamp();

    std::string resource_link =
        "dbs/" + config["cosmos_db_name"].get<std::string>() + "/colls/" +
        config["cosmos_container"].get<std::string>() + "/docs/" + track_id;

    SPDLOG_TRACE("Resource link constructed: {}", resource_link);

    std::string http_verb = "GET";
    std::string resource_type = "docs";
    std::string auth_token =
        create_auth_token(http_verb, resource_type, resource_link, timestamp);

    std::string url =
        config["cosmos_uri"].get<std::string>() + "/" + resource_link;

    SPDLOG_TRACE("Request URL constructed: {}", url);

    Curl curl;

    // Set headers using Curl::set_header
    SPDLOG_TRACE("Setting HTTP headers");
    curl.set_header("Accept: application/json");
    curl.set_header("x-ms-date: " + timestamp);
    curl.set_header("x-ms-version: " + cosmos_api_version);
    curl.set_header("authorization: " + auth_token);
    curl.set_header("x-ms-documentdb-partitionkey: [\"" + track_id + "\"]");

    try {
        SPDLOG_TRACE("Configuring cURL options");
        curl.set_option(CURLOPT_URL, url);
        curl.set_option(CURLOPT_CUSTOMREQUEST, "GET");
        curl.set_option(CURLOPT_TIMEOUT, 30L);
        curl.set_option(CURLOPT_FOLLOWLOCATION, 1L);

        SPDLOG_TRACE("Performing HTTP request");
        curl.perform();

        long response_code = curl.get_info<long>(CURLINFO_RESPONSE_CODE);
        SPDLOG_TRACE("HTTP response received with status code: {}",
                     response_code);

        if (response_code == 404) {
            SPDLOG_TRACE("Item not found (404) for track_id: {}", track_id);
            return std::nullopt; // Not found
        }

        if (response_code != 200) {
            SPDLOG_TRACE("HTTP request failed with unexpected status: {}",
                         response_code);
            throw std::runtime_error("HTTP request failed with status: " +
                                     std::to_string(response_code));
        }

        const std::string &response = curl.get_response();
        SPDLOG_TRACE("HTTP response body received, length: {}",
                     response.length());

        if (response.empty()) {
            SPDLOG_TRACE("Empty response body received");
            return std::nullopt;
        }

        SPDLOG_TRACE("Parsing JSON response");
        auto document = nlohmann::json::parse(response);
        SPDLOG_TRACE("JSON parsed successfully, document contains {} fields",
                     document.size());
        return document;

    } catch (const std::exception &e) {
        SPDLOG_TRACE("Exception occurred during fetch: {}", e.what());
        throw;
    }
}

std::string CosmosDB::generate_random_track_id() const {
    SPDLOG_TRACE("Generating random track ID");

    int min_value = config.value("min_value", 1);
    int max_value = config.value("max_value", 2000000);

    SPDLOG_TRACE("Track ID range: {} to {}", min_value, max_value);

    const auto rand_num =
        Utilities::generate_unique_ints(1, min_value, max_value);

    std::string track_id = std::to_string(rand_num->front());
    SPDLOG_TRACE("Generated track ID: {}", track_id);
    return track_id;
}

bool CosmosDB::is_not_found_response(const nlohmann::json &document) const {
    SPDLOG_TRACE("Checking if document is a 'NotFound' response");

    if (!document.contains("code") || !document["code"].is_string()) {
        SPDLOG_TRACE(
            "Document does not contain 'code' field or code is not string");
        return false;
    }

    std::string code = document["code"].get<std::string>();
    bool is_not_found = code == "NotFound";

    SPDLOG_TRACE("Document code: {}, is_not_found: {}", code, is_not_found);
    return is_not_found;
}

std::optional<nlohmann::json> CosmosDB::get_item() {
    SPDLOG_TRACE("Starting get_item operation");

    int max_retries = config.value("max_retries", 5);
    SPDLOG_TRACE("Maximum retries configured: {}", max_retries);

    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        SPDLOG_TRACE("Attempt {} of {}", attempt, max_retries);

        std::string track_id = generate_random_track_id();

        try {
            auto document = fetch_cosmos_item(track_id);

            if (!document.has_value()) {
                SPDLOG_TRACE(
                    "No document returned for track_id: {}, continuing to next attempt",
                    track_id);
                continue; // Not found, try again
            }

            if (is_not_found_response(document.value())) {
                SPDLOG_TRACE(
                    "NotFound response received for track_id: {}, continuing to next attempt",
                    track_id);
                continue; // Not found response, try again
            }

            SPDLOG_TRACE("Successfully found document for track_id: {}",
                         track_id);
            return document;

        } catch (const std::exception &e) {
            SPDLOG_TRACE("Exception on attempt {} for track_id {}: {}", attempt,
                         track_id, e.what());
            if (attempt == max_retries) {
                SPDLOG_TRACE("Max retries reached, re-throwing exception");
                throw; // Re-throw on last attempt
            }
            SPDLOG_TRACE("Continuing to next attempt after exception");
            // Continue trying on other attempts
        }
    }

    SPDLOG_TRACE("All {} attempts exhausted, returning nullopt", max_retries);
    return std::nullopt;
}

std::string CosmosDB::url_encode(const std::string &value) const {
    SPDLOG_TRACE("URL encoding string of length: {}", value.length());

    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    int encoded_chars = 0;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << int(c);
            encoded_chars++;
        }
    }

    std::string result = escaped.str();
    SPDLOG_TRACE(
        "URL encoding completed, {} characters encoded, final length: {}",
        encoded_chars, result.length());
    return result;
}
