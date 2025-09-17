#include <stdexcept>

#include "rapidyenc.hpp"
#include "rapidyenc/rapidyenc.h"

RapidYenc::RapidYenc() : initialized(false) {
    // Initialize encoding functionality
    rapidyenc_encode_init();

    // Initialize decoding functionality
    rapidyenc_decode_init();

    initialized = true;
}

RapidYenc::~RapidYenc() = default;

RapidYenc::RapidYenc(RapidYenc &&other) noexcept
    : initialized(other.initialized) {
    other.initialized = false;
}

RapidYenc &RapidYenc::operator=(RapidYenc &&other) noexcept {
    if (this != &other) {
        initialized = other.initialized;
        other.initialized = false;
    }
    return *this;
}

void RapidYenc::ensure_initialized() const {
    if (!initialized) {
        throw std::runtime_error("RapidYenc instance not properly initialized");
    }
}

// Encode methods

std::vector<std::byte>
RapidYenc::encode(const std::vector<std::byte> &input) const {
    return encode(input.data(), input.size());
}

std::vector<std::byte> RapidYenc::encode(const std::byte *data,
                                         size_t size) const {
    ensure_initialized();

    if (data == nullptr || size == 0) {
        return {};
    }

    size_t max_output_size = get_max_encoded_length(size);
    std::vector<std::byte> output(max_output_size);

    size_t actual_output_size =
        rapidyenc_encode(static_cast<const void *>(data),
                         static_cast<void *>(output.data()), size);

    output.resize(actual_output_size);
    return output;
}

std::string RapidYenc::encode_string(const std::string &input) const {
    auto encoded =
        encode(reinterpret_cast<const std::byte *>(input.data()), input.size());
    return std::string(reinterpret_cast<const char *>(encoded.data()),
                       encoded.size());
}

std::string
RapidYenc::encode_to_string(const std::vector<std::byte> &input) const {
    auto encoded = encode(input.data(), input.size());
    return std::string(reinterpret_cast<const char *>(encoded.data()),
                       encoded.size());
}

// Decode methods

std::vector<std::byte>
RapidYenc::decode(const std::vector<std::byte> &input) const {
    return decode(input.data(), input.size());
}

std::vector<std::byte> RapidYenc::decode(const std::byte *data,
                                         size_t size) const {
    ensure_initialized();

    if (data == nullptr || size == 0) {
        return {};
    }

    std::vector<std::byte> output(size);

    size_t actual_output_size =
        rapidyenc_decode(static_cast<const void *>(data),
                         static_cast<void *>(output.data()), size);

    output.resize(actual_output_size);
    return output;
}

std::string RapidYenc::decode_string(const std::string &input) const {
    auto decoded =
        decode(reinterpret_cast<const std::byte *>(input.data()), input.size());
    return std::string(reinterpret_cast<const char *>(decoded.data()),
                       decoded.size());
}

std::vector<std::byte>
RapidYenc::decode_from_string(const std::string &input) const {
    return decode(reinterpret_cast<const std::byte *>(input.data()),
                  input.size());
}

// Utility methods

size_t RapidYenc::get_max_encoded_length(size_t input_size, int line_size) {
    return rapidyenc_encode_max_length(input_size, line_size);
}