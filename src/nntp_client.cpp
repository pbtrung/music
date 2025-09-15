#include <algorithm>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "nntp_client.hpp"

// Static member initialization
SSL_CTX *NntpClient::ssl_context = nullptr;
bool NntpClient::ssl_initialized = false;

NntpClient::ConnectionState::~ConnectionState() {
    SPDLOG_TRACE("Destroying connection state");
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (socket_fd != -1) {
        close(socket_fd);
    }
}

void NntpClient::initialize_ssl() {
    if (ssl_initialized)
        return;

    SPDLOG_TRACE("Initializing SSL");
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    ssl_context = SSL_CTX_new(TLS_client_method());
    if (!ssl_context) {
        ERR_print_errors_fp(stderr);
        throw std::runtime_error("Failed to create SSL context");
    }

    SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ssl_context);

    ssl_initialized = true;
    SPDLOG_TRACE("SSL initialized successfully");
}

void NntpClient::cleanup_ssl() {
    SPDLOG_TRACE("Cleaning up SSL");
    if (ssl_context) {
        SSL_CTX_free(ssl_context);
        ssl_context = nullptr;
    }
    EVP_cleanup();
    ssl_initialized = false;
}

std::unique_ptr<NntpClient::ConnectionState>
NntpClient::create_connection(const NntpConnection &conn) {
    SPDLOG_TRACE("Creating connection to {}:{}", conn.hostname, conn.port);

    if (conn.use_ssl) {
        initialize_ssl();
    }

    auto state = std::make_unique<ConnectionState>();

    // Resolve hostname
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result;
    int status =
        getaddrinfo(conn.hostname.c_str(), std::to_string(conn.port).c_str(),
                    &hints, &result);
    if (status != 0) {
        SPDLOG_TRACE("getaddrinfo failed: {}", gai_strerror(status));
        throw std::runtime_error("Failed to resolve hostname: " +
                                 std::string(gai_strerror(status)));
    }

    // Create socket
    state->socket_fd =
        socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (state->socket_fd == -1) {
        freeaddrinfo(result);
        SPDLOG_TRACE("Socket creation failed: {}", strerror(errno));
        throw std::runtime_error("Socket creation failed: " +
                                 std::string(strerror(errno)));
    }

    // Connect
    if (connect(state->socket_fd, result->ai_addr, result->ai_addrlen) == -1) {
        freeaddrinfo(result);
        SPDLOG_TRACE("Connection failed: {}", strerror(errno));
        throw std::runtime_error("Connection failed: " +
                                 std::string(strerror(errno)));
    }

    freeaddrinfo(result);
    SPDLOG_TRACE("Socket connected successfully");

    // Setup SSL if required
    if (conn.use_ssl) {
        SPDLOG_TRACE("Setting up SSL connection");
        state->ssl = SSL_new(ssl_context);
        if (!state->ssl) {
            ERR_print_errors_fp(stderr);
            throw std::runtime_error("Failed to create SSL structure");
        }

        SSL_set_fd(state->ssl, state->socket_fd);

        if (SSL_connect(state->ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            throw std::runtime_error("SSL handshake failed");
        }
        SPDLOG_TRACE("SSL connection established");
    }

    state->connected = true;

    // Read initial server response
    auto response = receive_response(state.get());
    if (!check_response_code(response, "200")) {
        SPDLOG_TRACE("Server initial response failed: {}", response);
        throw std::runtime_error("Server connection failed: " + response);
    }

    SPDLOG_TRACE("Connection established, server response: {}", response);
    return state;
}

void NntpClient::send_command(ConnectionState *state,
                              const std::string &command) {
    if (!state || !state->connected) {
        throw std::runtime_error("Connection not established");
    }

    // Mask sensitive commands in logs
    std::string log_command = command;
    if (command.starts_with("AUTHINFO USER ")) {
        log_command = "AUTHINFO USER [MASKED]";
    } else if (command.starts_with("AUTHINFO PASS ")) {
        log_command = "AUTHINFO PASS [MASKED]";
    }

    SPDLOG_TRACE("Sending command: {}", log_command);
    std::string full_command = command + "\r\n";
    const char *data = full_command.c_str();
    size_t len = full_command.length();

    ssize_t sent = 0;
    if (state->ssl) {
        sent = SSL_write(state->ssl, data, static_cast<int>(len));
    } else {
        sent = send(state->socket_fd, data, len, 0);
    }

    if (sent != static_cast<ssize_t>(len)) {
        SPDLOG_TRACE(
            "Failed to send command completely. Sent: {}, Expected: {}", sent,
            len);
        throw std::runtime_error("Failed to send command: " + log_command);
    }
}

std::string NntpClient::receive_response(ConnectionState *state) {
    if (!state || !state->connected) {
        throw std::runtime_error("Connection not established");
    }

    std::string response;
    char buffer[1024];

    while (true) {
        ssize_t received = 0;
        if (state->ssl) {
            received = SSL_read(state->ssl, buffer, sizeof(buffer) - 1);
        } else {
            received = recv(state->socket_fd, buffer, sizeof(buffer) - 1, 0);
        }

        if (received <= 0) {
            SPDLOG_TRACE("Failed to receive response, bytes: {}", received);
            throw std::runtime_error("Failed to receive response from server");
        }

        buffer[received] = '\0';
        response += buffer;

        // Check for end of line
        size_t pos = response.find("\r\n");
        if (pos != std::string::npos) {
            std::string result = response.substr(0, pos);
            SPDLOG_TRACE("Received response: {}", result);
            return result;
        }
    }
}

std::vector<std::string>
NntpClient::receive_multiline_response(ConnectionState *state) {
    SPDLOG_TRACE("Receiving multiline response");
    std::vector<std::string> lines;

    while (true) {
        auto line = receive_response(state);

        // End of multiline response is a single dot
        if (line == ".") {
            SPDLOG_TRACE("End of multiline response received");
            break;
        }

        // Remove dot-stuffing (lines starting with .. become .)
        if (line.length() > 0 && line[0] == '.' && line.length() > 1 &&
            line[1] == '.') {
            line = line.substr(1);
        }

        lines.push_back(line);
    }

    SPDLOG_TRACE("Received {} lines in multiline response", lines.size());
    return lines;
}

bool NntpClient::check_response_code(const std::string &response,
                                     const std::string &expected_code) {
    bool matches = response.length() >= expected_code.length() &&
                   response.substr(0, expected_code.length()) == expected_code;
    SPDLOG_TRACE("Response code check: expected '{}', got '{}', matches: {}",
                 expected_code, response.substr(0, 3), matches);
    return matches;
}

void NntpClient::close_connection(std::unique_ptr<ConnectionState> state) {
    if (state && state->connected) {
        SPDLOG_TRACE("Closing connection");
        try {
            send_command(state.get(), "QUIT");
        } catch (const std::exception &e) {
            SPDLOG_TRACE("Failed to send QUIT command: {}", e.what());
        }
    }
}

NntpMessage NntpClient::parse_message(const std::vector<std::string> &lines) {
    SPDLOG_TRACE("Parsing message with {} lines", lines.size());
    NntpMessage message;
    bool in_body = false;

    for (const auto &line : lines) {
        if (!in_body && line.empty()) {
            in_body = true;
            SPDLOG_TRACE("Switched to body parsing");
            continue;
        }

        if (in_body) {
            if (!message.body.empty())
                message.body += "\n";
            message.body += line;
        } else {
            message.headers.push_back(line);

            // Parse common headers
            if (line.starts_with("Message-ID: ")) {
                message.message_id = line.substr(12);
                SPDLOG_TRACE("Found Message-ID: {}", message.message_id);
            } else if (line.starts_with("Subject: ")) {
                message.subject = line.substr(9);
                SPDLOG_TRACE("Found Subject: {}", message.subject);
            } else if (line.starts_with("From: ")) {
                message.from = line.substr(6);
                SPDLOG_TRACE("Found From: {}", message.from);
            } else if (line.starts_with("Date: ")) {
                message.date = line.substr(6);
                SPDLOG_TRACE("Found Date: {}", message.date);
            } else if (line.starts_with("Newsgroups: ")) {
                message.newsgroups = line.substr(12);
                SPDLOG_TRACE("Found Newsgroups: {}", message.newsgroups);
            }
        }
    }

    SPDLOG_TRACE("Message parsed: subject='{}', from='{}'", message.subject,
                 message.from);
    return message;
}

void NntpClient::authenticate_connection(ConnectionState *state,
                                         const NntpConnection &connection) {
    if (connection.username.empty())
        return;

    SPDLOG_TRACE("Authenticating user: {}", connection.username);

    send_command(state, "AUTHINFO USER " + connection.username);
    auto response = receive_response(state);
    if (!check_response_code(response, "381")) {
        throw std::runtime_error("Authentication failed at username step: " +
                                 response);
    }

    send_command(state, "AUTHINFO PASS " + connection.password);
    response = receive_response(state);
    if (!check_response_code(response, "281")) {
        throw std::runtime_error("Authentication failed at password step: " +
                                 response);
    }

    SPDLOG_TRACE("Authentication successful");
}

bool NntpClient::authenticate(const NntpConnection &connection) {
    SPDLOG_TRACE("Testing authentication for user: {}", connection.username);

    try {
        auto state = create_connection(connection);
        authenticate_connection(state.get(), connection);
        close_connection(std::move(state));
        return true;
    } catch (const std::exception &e) {
        SPDLOG_TRACE("Authentication test failed: {}", e.what());
        return false;
    }
}

NntpMessage NntpClient::get_message(const NntpConnection &connection,
                                    const std::string &message_id) {
    SPDLOG_TRACE("Getting message: {}", message_id);

    auto state = create_connection(connection);
    authenticate_connection(state.get(), connection);

    // Request article
    std::string command = "ARTICLE ";
    if (message_id.front() == '<' && message_id.back() == '>') {
        command += message_id;
    } else {
        command += "<" + message_id + ">";
    }

    send_command(state.get(), command);
    auto response = receive_response(state.get());
    if (!check_response_code(response, "220")) {
        throw std::runtime_error("Failed to retrieve article: " + response);
    }

    auto lines = receive_multiline_response(state.get());
    close_connection(std::move(state));

    return parse_message(lines);
}

void NntpClient::post_message(const NntpConnection &connection,
                              const NntpMessage &message) {
    SPDLOG_TRACE("Posting message with subject: {}", message.subject);

    auto state = create_connection(connection);
    authenticate_connection(state.get(), connection);

    // Start posting
    send_command(state.get(), "POST");
    auto response = receive_response(state.get());
    if (!check_response_code(response, "340")) {
        throw std::runtime_error("Server rejected POST command: " + response);
    }

    // Send headers
    for (const auto &header : message.headers) {
        send_command(state.get(), header);
    }

    // Send common headers if not already present
    bool has_subject = false, has_from = false, has_newsgroups = false,
         has_message_id = false, has_date = false;
    for (const auto &header : message.headers) {
        if (header.starts_with("Subject: "))
            has_subject = true;
        else if (header.starts_with("From: "))
            has_from = true;
        else if (header.starts_with("Newsgroups: "))
            has_newsgroups = true;
        else if (header.starts_with("Message-ID: "))
            has_message_id = true;
        else if (header.starts_with("Date: "))
            has_date = true;
    }

    if (!has_subject && !message.subject.empty()) {
        send_command(state.get(), "Subject: " + message.subject);
    }
    if (!has_from && !message.from.empty()) {
        send_command(state.get(), "From: " + message.from);
    }
    if (!has_newsgroups && !message.newsgroups.empty()) {
        send_command(state.get(), "Newsgroups: " + message.newsgroups);
    }
    if (!has_message_id && !message.message_id.empty()) {
        send_command(state.get(), "Message-ID: " + message.message_id);
    }

    // Generate date header if not present
    if (!has_date) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream date_stream;
        date_stream << std::put_time(std::gmtime(&time_t),
                                     "Date: %a, %d %b %Y %H:%M:%S GMT");
        send_command(state.get(), date_stream.str());
    }

    // Empty line to separate headers from body
    send_command(state.get(), "");

    // Send body
    std::stringstream body_stream(message.body);
    std::string line;
    while (std::getline(body_stream, line)) {
        // Dot-stuff lines that start with a dot
        if (!line.empty() && line[0] == '.') {
            line = "." + line;
        }
        send_command(state.get(), line);
    }

    // End message with a single dot
    send_command(state.get(), ".");

    response = receive_response(state.get());
    if (!check_response_code(response, "240")) {
        throw std::runtime_error("Message posting failed: " + response);
    }

    close_connection(std::move(state));
    SPDLOG_TRACE("Message posted successfully");
}
