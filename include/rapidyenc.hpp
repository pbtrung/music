#ifndef RAPIDYENC_HPP
#define RAPIDYENC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class RapidYenc {
  public:
    // Constructor - initializes the library
    RapidYenc();

    // Destructor - ensures proper cleanup
    ~RapidYenc();

    // Delete copy constructor and copy assignment operator
    RapidYenc(const RapidYenc &) = delete;
    RapidYenc &operator=(const RapidYenc &) = delete;

    // Move constructor and move assignment operator
    RapidYenc(RapidYenc &&other) noexcept;
    RapidYenc &operator=(RapidYenc &&other) noexcept;

    // Encode methods
    std::vector<std::byte> encode(const std::vector<std::byte> &input) const;
    std::vector<std::byte> encode(const std::byte *data, size_t size) const;
    std::string encode_string(const std::string &input) const;

    // Decode methods
    std::vector<std::byte> decode(const std::vector<std::byte> &input) const;
    std::vector<std::byte> decode(const std::byte *data, size_t size) const;
    std::string decode_string(const std::string &input) const;

    // Utility methods
    static size_t get_max_encoded_length(size_t input_size,
                                         int line_size = 128);

  private:
    bool initialized;

    // Helper methods
    void ensure_initialized() const;
};

#endif // RAPIDYENC_HPP