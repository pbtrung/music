#pragma once

#include <concepts>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Utilities {
  private:
    static bool validate_string(std::string_view str, size_t max_len) noexcept;
    static bool validate_path(std::string_view component) noexcept;
    static bool validate_filename(std::string_view filename) noexcept;

  public:
    Utilities() = delete;
    Utilities(const Utilities &) = delete;
    Utilities &operator=(const Utilities &) = delete;
    Utilities(Utilities &&) = delete;
    Utilities &operator=(Utilities &&) = delete;
    ~Utilities() = delete;

    static void trim_spaces(std::string &str) noexcept;
    static std::string format_time(int seconds) noexcept;
    static std::optional<std::filesystem::path>
    make_path(std::string_view directory, std::string_view filename) noexcept;
    static void to_lower(std::string &str) noexcept;
    static std::optional<std::string>
    get_extension(std::string_view filename) noexcept;
    static std::optional<std::string>
    generate_filename(std::string_view original_filename) noexcept;
    static std::optional<std::vector<int>>
    generate_unique_ints(int count, int min_val, int max_val) noexcept;
    static std::string generate_random_string(size_t length) noexcept;
    static std::string format_commas(long num) noexcept;

    template <std::integral T>
    static std::string format_with_commas(T num) noexcept;

    static constexpr size_t max_path_length = 4096;
    static constexpr size_t max_filename_length = 255;
    static constexpr size_t max_extension_length = 10;
    static constexpr size_t max_time_string_length = 32;
    static constexpr size_t min_random_string_length = 1;
    static constexpr size_t max_random_string_length = 256;
    static constexpr size_t default_filename_length = 25;
};

template <std::integral T>
std::string Utilities::format_with_commas(T num) noexcept {
    std::string str = std::to_string(num);
    std::string result;
    result.reserve(str.length() + str.length() / 3);

    int count = 0;
    for (auto it = str.rbegin(); it != str.rend(); ++it) {
        if (count == 3) {
            result += ',';
            count = 0;
        }
        result += *it;
        ++count;
    }

    std::reverse(result.begin(), result.end());
    return result;
}
