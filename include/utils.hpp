#ifndef UTILS_HPP
#define UTILS_HPP

#include <blake2.h>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

class Utils {
  public:
    static void toLowercase(std::string &str);
    static std::string getExtension(std::string_view text);
    static std::string formatTime(std::chrono::seconds seconds);
    static std::array<uint8_t, BLAKE2S_OUTBYTES>
    getBlake2Hash(const std::string &filename);
    template <size_t n>
    static bool compareHashes(
        const std::array<std::array<uint8_t, BLAKE2S_OUTBYTES>, n> &hashes) {
        if (n <= 1) {
            return true;
        }
        for (size_t i = 1; i < n; ++i) {
            if (!std::equal(hashes[0].begin(), hashes[0].end(),
                            hashes[i].begin())) {
                return false;
            }
        }
        return true;
    }

  private:
    using Pcre2CodePtr =
        std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;
    using Pcre2MatchDataPtr =
        std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>;

    static Pcre2CodePtr compilePattern(std::string_view pattern);
    static Pcre2MatchDataPtr createMatchData(const pcre2_code *pattern);
};

#endif // UTILS_HPP
