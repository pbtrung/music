#ifndef UTILS_HPP
#define UTILS_HPP

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

  private:
    using Pcre2CodePtr =
        std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;
    using Pcre2MatchDataPtr =
        std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>;

    static Pcre2CodePtr compilePattern(std::string_view pattern);
    static Pcre2MatchDataPtr createMatchData(const pcre2_code *pattern);
};

#endif // UTILS_HPP
