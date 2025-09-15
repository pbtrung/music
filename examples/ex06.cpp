#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "nntp_client.hpp"

int main() {
    // Set up logging
    spdlog::set_level(spdlog::level::trace);
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);

    NntpConnection conn{.hostname = "host",
                        .port = 563,
                        .username = "username",
                        .password = "password",
                        .use_ssl = true};

    try {
        // Test authentication
        if (NntpClient::authenticate(conn)) {
            spdlog::info("Authentication successful");
        }

        // // Get a message
        // auto message = NntpClient::get_message(conn,
        // "message-id@example.com"); spdlog::info("Retrieved message: {}",
        // message.subject);

        // // Post a message
        // NntpMessage new_msg;
        // new_msg.subject = "Test Subject";
        // new_msg.from = "user@example.com";
        // new_msg.newsgroups = "test.group";
        // new_msg.body = "Test message body";

        // NntpClient::post_message(conn, new_msg);
        // spdlog::info("Message posted successfully");

    } catch (const std::exception &e) {
        spdlog::error("NNTP operation failed: {}", e.what());
    }

    return 0;
}
