#include "utils.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <iostream>

std::string utils::get_time(double seconds) {
    int hours = static_cast<int>(seconds / 3600);
    seconds -= hours * 3600;
    int minutes = static_cast<int>(seconds / 60);
    seconds -= minutes * 60;
    int milliseconds =
        static_cast<int>((seconds - static_cast<int>(seconds)) * 1000);

    std::string time = "00:00.000";
    if (hours > 0) {
        time = fmt::format("{:02}:{:02}:{:02}.{:03}", hours, minutes,
                           static_cast<int>(seconds), milliseconds);
    } else {
        time = fmt::format("{:02}:{:02}.{:03}", minutes,
                           static_cast<int>(seconds), milliseconds);
    }
    return time;
}

void utils::to_lowercase(std::string &str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
}

std::string utils::get_extension(const std::string &text) {
    Pcre2Pattern pattern("(.*)\\.(opus|mp3|m4a)$");
    Pcre2MatchData match_data(pattern.get());

    int rc =
        pcre2_match(pattern.get(), reinterpret_cast<PCRE2_SPTR>(text.c_str()),
                    text.length(), 0, 0, match_data.get(), nullptr);

    if (rc >= 3) {
        auto *ovector = pcre2_get_ovector_pointer(match_data.get());
        std::string ext = text.substr(ovector[4], ovector[5] - ovector[4]);
        to_lowercase(ext);
        return ext;
    }

    return "";
}