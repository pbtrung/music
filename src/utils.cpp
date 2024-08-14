#include "utils.hpp"
#include "fmtlog-inl.hpp"
#include <algorithm>
#include <chrono>
#include <fmt/core.h>
#include <stdexcept>

Utils::Pcre2CodePtr Utils::compilePattern(std::string_view pattern) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                   PCRE2_ZERO_TERMINATED, PCRE2_CASELESS,
                                   &errcode, &erroffset, nullptr);

    if (!re) {
        loge("PCRE2 compilation failed at offset {}", erroffset);
        throw std::runtime_error(
            fmt::format("PCRE2 compilation failed at offset {}", erroffset));
    }
    return Pcre2CodePtr(re, pcre2_code_free);
}

Utils::Pcre2MatchDataPtr Utils::createMatchData(const pcre2_code *pattern) {
    pcre2_match_data *match_data =
        pcre2_match_data_create_from_pattern(pattern, nullptr);
    if (!match_data) {
        loge("Failed to create match data");
        throw std::runtime_error("Failed to create match data");
    }
    return Pcre2MatchDataPtr(match_data, pcre2_match_data_free);
}

void Utils::toLowercase(std::string &str) {
    std::ranges::transform(str, str.begin(),
                           [](unsigned char c) { return std::tolower(c); });
}

std::string Utils::getExtension(std::string_view text) {
    auto pattern = compilePattern("(.*)\\.(opus|mp3|m4a)$");
    auto matchData = createMatchData(pattern.get());

    int rc =
        pcre2_match(pattern.get(), reinterpret_cast<PCRE2_SPTR>(text.data()),
                    text.length(), 0, PCRE2_ANCHORED, matchData.get(), nullptr);

    if (rc >= 3) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData.get());
        std::string_view ext(text.data() + ovector[4], ovector[5] - ovector[4]);
        std::string result(ext);
        toLowercase(result);
        return result;
    } else {
        loge("Failed to find extension from {}", text);
        throw std::runtime_error(
            fmt::format("Failed to find extension from {}", text));
    }
}

std::string Utils::formatTime(std::chrono::seconds seconds) {
    using namespace std::chrono;

    auto hrs = duration_cast<hours>(seconds);
    auto mins = duration_cast<minutes>(seconds - hrs);
    auto secs = seconds - hrs - mins;

    if (hrs.count() > 0) {
        return fmt::format("{:02d}:{:02d}:{:02d}", hrs.count(), mins.count(),
                           secs.count());
    } else {
        return fmt::format("{:02d}:{:02d}", mins.count(), secs.count());
    }
}
