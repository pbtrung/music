#ifndef RAPIDYENC_HPP
#define RAPIDYENC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class RapidYenc {
  public:
    // Static encode methods
    static std::vector<std::byte> encode(const std::vector<std::byte> &input);
    static std::vector<std::byte> encode(const std::byte *data, size_t size);
    static std::string encode_string(const std::string &input);
    static std::string encode_to_string(const std::vector<std::byte> &input);

    // Static decode methods
    static std::vector<std::byte> decode(const std::vector<std::byte> &input);
    static std::vector<std::byte> decode(const std::byte *data, size_t size);
    static std::string decode_string(const std::string &input);
    static std::vector<std::byte> decode_from_string(const std::string &input);

    // Utility methods
    static size_t get_max_encoded_length(size_t input_size,
                                         int line_size = 128);

    // Library initialization methods
    static bool initialize();
    static void cleanup();
    static bool is_initialized();

  private:
    static bool s_initialized;

    // Helper methods
    static void ensure_initialized();
    static bool safe_initialize();
};

#endif // RAPIDYENC_HPP