#pragma once

#include <arpa/inet.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

struct NntpMessage {
    std::string message_id;
    std::string subject;
    std::string from;
    std::string date;
    std::string newsgroups;
    std::vector<std::string> headers;
    std::string body;
};

struct NntpConnection {
    std::string hostname;
    int port;
    std::string username;
    std::string password;
    bool use_ssl;
};

class NntpClient {
  private:
    static SSL_CTX *ssl_context;
    static bool ssl_initialized;

    struct ConnectionState {
        int socket_fd;
        SSL *ssl;
        bool connected;
        std::string receive_buffer;

        ConnectionState() : socket_fd(-1), ssl(nullptr), connected(false) {}
        ~ConnectionState();
    };

    static void initialize_ssl();
    static void cleanup_ssl();
    static std::unique_ptr<ConnectionState>
    create_connection(const NntpConnection &conn);
    static void send_command(ConnectionState *state,
                             const std::string &command);
    static std::string receive_response(ConnectionState *state);
    static std::vector<std::string>
    receive_multiline_response(ConnectionState *state);
    static bool check_response_code(const std::string &response,
                                    const std::string &expected_code);
    static void close_connection(std::unique_ptr<ConnectionState> state);
    static NntpMessage parse_message(const std::vector<std::string> &lines);
    static void authenticate_connection(ConnectionState *state,
                                        const NntpConnection &connection);

  public:
    static bool authenticate(const NntpConnection &connection);
    static NntpMessage get_message(const NntpConnection &connection,
                                   const std::string &message_id);
    static void post_message(const NntpConnection &connection,
                             const NntpMessage &message);
};