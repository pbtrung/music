#include <stdexcept>

#include <fmt/base.h>

#include "rapidyenc.hpp"

int main() {
    try {
        RapidYenc yenc;

        // Simple usage
        std::string input = "Hello, World!";
        auto encoded = yenc.encode_string(input);
        fmt::println("encoded: {}", encoded);
        auto decoded = yenc.decode_string(encoded);
        fmt::println("decoded: {}", decoded);
        if (input != decoded) {
            throw std::runtime_error("input != decoded");
        }

        // Binary data usage
        std::vector<std::byte> binary_data = {std::byte{0x48}, std::byte{0x65},
                                              std::byte{0x6c}, std::byte{0x6c},
                                              std::byte{0x6f}};
        auto encoded_binary = yenc.encode(binary_data);
        auto decoded_binary = yenc.decode(encoded_binary);

    } catch (const std::exception &e) {
        fmt::println("exception: {}", e.what());
    }

    return 0;
}
