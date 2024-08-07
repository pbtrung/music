#include "utils.hpp"
#include <algorithm>

// Utils Implementation

void Utils::toLowercase(std::string &str) {
    std::ranges::transform(
        str, str.begin(), [](unsigned char c) { return std::tolower(c); });
}

std::string Utils::getExtension(std::string_view text) {
    Pcre2Pattern pattern("(.*)\\.(opus|mp3|m4a)$");
    Pcre2MatchData matchData(pattern);

    int rc = pcre2_match(pattern.get(),
                         reinterpret_cast<PCRE2_SPTR>(text.data()),
                         text.length(),
                         0,
                         PCRE2_ANCHORED,
                         matchData.get(),
                         nullptr);

    if (rc >= 3) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData.get());
        std::string_view ext(text.data() + ovector[4], ovector[5] - ovector[4]);
        std::string result(ext);
        toLowercase(result);
        return result;
    }
    return "";
}

std::string Utils::formatTime(std::chrono::seconds seconds) {
    using namespace std::chrono;

    auto hrs = duration_cast<hours>(seconds);
    auto mins = duration_cast<minutes>(seconds - hrs);
    auto secs = seconds - hrs - mins;

    if (hrs.count() > 0) {
        return std::format(
            "{:02d}:{:02d}:{:02d}", hrs.count(), mins.count(), secs.count());
    } else {
        return std::format("{:02d}:{:02d}", mins.count(), secs.count());
    }
}

// Pcre2Pattern Implementation

Pcre2Pattern::Pcre2Pattern(std::string_view pattern) {
    int errcode;
    PCRE2_SIZE erroffset;
    re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                       PCRE2_ZERO_TERMINATED,
                       PCRE2_CASELESS,
                       &errcode,
                       &erroffset,
                       nullptr);
    if (!re) {
        throw std::runtime_error("PCRE2 compilation failed");
    }
}

Pcre2Pattern::~Pcre2Pattern() {
    pcre2_code_free(re);
}

pcre2_code *Pcre2Pattern::get() const {
    return re;
}

// Pcre2MatchData Implementation

Pcre2MatchData::Pcre2MatchData(const Pcre2Pattern &pattern) {
    match_data = pcre2_match_data_create_from_pattern(pattern.get(), nullptr);
    if (!match_data) {
        throw std::runtime_error("Failed to create match data");
    }
}

Pcre2MatchData::~Pcre2MatchData() {
    pcre2_match_data_free(match_data);
}

pcre2_match_data *Pcre2MatchData::get() const {
    return match_data;
}