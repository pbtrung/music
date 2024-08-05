#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <stdexcept>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

class utils {
  public:
    static void to_lowercase(std::string &str);
    static std::string get_extension(const std::string &text);
    static std::string get_time(double seconds);
};

class Pcre2Pattern {
  public:
    Pcre2Pattern(const std::string &pattern) {
        int errcode;
        PCRE2_SIZE erroffset;
        re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()),
                           PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &errcode,
                           &erroffset, nullptr);
        if (!re) {
            throw std::runtime_error("PCRE2 compilation failed");
        }
    }

    ~Pcre2Pattern() {
        if (re) {
            pcre2_code_free(re);
        }
    }

    pcre2_code *get() const { return re; }

  private:
    pcre2_code *re = nullptr;
};

class Pcre2MatchData {
  public:
    Pcre2MatchData(pcre2_code *pattern) {
        match_data = pcre2_match_data_create_from_pattern(pattern, nullptr);
        if (!match_data) {
            throw std::runtime_error("Failed to create match data");
        }
    }

    ~Pcre2MatchData() {
        if (match_data) {
            pcre2_match_data_free(match_data);
        }
    }

    pcre2_match_data *get() const { return match_data; }

  private:
    pcre2_match_data *match_data = nullptr;
};

#endif // UTILS_HPP
