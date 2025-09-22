#include <stdexcept>

#include <fmt/base.h>

#include "rapidyenc.hpp"

int main() {
    try {
        // Optional
        if (!RapidYenc::initialize()) {
            throw std::runtime_error("Failed to initialize RapidYenc library");
        }

        // Simple usage
        std::string input = "Hello, World!";
        auto encoded = RapidYenc::encode_string(input);
        fmt::println("encoded: {}", encoded);
        auto decoded = RapidYenc::decode_string(encoded);
        fmt::println("decoded: {}", decoded);
        if (input != decoded) {
            throw std::runtime_error("input != decoded");
        }

        // Binary data usage
        std::vector<std::byte> binary_data = {std::byte{0x48}, std::byte{0x65},
                                              std::byte{0x6c}, std::byte{0x6c},
                                              std::byte{0x6f}};
        auto encoded_binary = RapidYenc::encode(binary_data);
        auto decoded_binary = RapidYenc::decode(encoded_binary);

        // Verify binary data roundtrip
        if (binary_data != decoded_binary) {
            throw std::runtime_error("binary_data != decoded_binary");
        }
        fmt::println("Binary data roundtrip successful");

        // Optional
        RapidYenc::cleanup();

    } catch (const std::exception &e) {
        fmt::println("exception: {}", e.what());
        return 1;
    }

    return 0;
}