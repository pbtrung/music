#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <fmt/base.h>
#include <fmt/format.h>
#include <openssl/rand.h>

#include "cppcodec/base64_url_unpadded.hpp"
#include "nntp_client.hpp"
#include "rapidyenc.hpp"
#include "utils.hpp"

using base64_url_unpadded = cppcodec::base64_url_unpadded;

int main() {
    // Set up logging
    spdlog::set_level(spdlog::level::trace);
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);

    NntpConnection conn{.hostname = "",
                        .port = 563,
                        .username = "",
                        .password = "",
                        .use_ssl = true};

    try {
        // Test authentication
        if (NntpClient::authenticate(conn)) {
            spdlog::info("Authentication successful");
        }

        std::vector<std::byte> body(4999);
        if (RAND_bytes(reinterpret_cast<unsigned char *>(body.data()),
                       body.size()) != 1) {
            fmt::println("Error: RAND_bytes failed");
            return 1;
        }
        RapidYenc yenc;
        auto encoded_body = yenc.encode_to_string(body);

        std::vector<std::byte> hmac_key(32);
        if (RAND_bytes(reinterpret_cast<unsigned char *>(hmac_key.data()),
                       hmac_key.size()) != 1) {
            fmt::println("Error: RAND_bytes failed");
            return 1;
        }

        std::string hmac_key_str = base64_url_unpadded::encode(hmac_key);
        std::string orig_hmac = Utilities::hmac_sha3_256(hmac_key_str, body);
        std::string message_id = fmt::format(
            "<{}@{}>", orig_hmac, Utilities::generate_random_string(10));
        std::string from =
            fmt::format("{}@{}.{}", Utilities::generate_random_string(10),
                        Utilities::generate_random_string(10),
                        Utilities::generate_random_string(5));

        // Post a message
        NntpMessage new_msg;
        new_msg.subject = Utilities::generate_random_string(42);
        new_msg.from = from;
        new_msg.newsgroups = "alt.binaries.music";
        new_msg.body = encoded_body;
        new_msg.message_id = message_id;

        NntpClient::post_message(conn, new_msg);
        spdlog::info("Message posted successfully");

        // Get a message
        auto message = NntpClient::get_message(conn, message_id);
        auto decoded_binary = yenc.decode_from_string(message.body);

        if (body == decoded_binary) {
            spdlog::info("a == b");
        } else {
            spdlog::info("a != b");
        }

        std::string hmac =
            Utilities::hmac_sha3_256(hmac_key_str, decoded_binary);
        spdlog::info(orig_hmac);
        spdlog::info(hmac);

    } catch (const std::exception &e) {
        spdlog::error("NNTP operation failed: {}", e.what());
    }

    return 0;
}
