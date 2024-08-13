#ifndef UTILS_HPP
#define UTILS_HPP

#include <chrono>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

class Utils {
  public:
    // Converts the string to lowercase
    static void toLowercase(std::string &str);

    // Returns the file extension in lowercase, or an empty string if not found
    static std::string getExtension(std::string_view text);

    // Formats time in seconds to HH:MM:SS or MM:SS
    static std::string formatTime(std::chrono::seconds seconds);
};

class Pcre2Pattern {
  public:
    explicit Pcre2Pattern(std::string_view pattern);
    ~Pcre2Pattern();
    pcre2_code *get() const;

  private:
    pcre2_code *re = nullptr;
};

class Pcre2MatchData {
  public:
    explicit Pcre2MatchData(const Pcre2Pattern &pattern);
    ~Pcre2MatchData();
    pcre2_match_data *get() const;

  private:
    pcre2_match_data *match_data = nullptr;
};

#endif // UTILS_HPP