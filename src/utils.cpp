#include "utils.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <iostream>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

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
    pcre2_code *re;
    pcre2_match_data *match_data;
    int errcode;
    PCRE2_SIZE erroffset;
    PCRE2_SIZE *ovector;

    re = pcre2_compile((PCRE2_SPTR) "(.*)\\.(opus|mp3|m4a)$",
                       PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &errcode,
                       &erroffset, NULL);

    if (!re) {
        throw std::runtime_error("PCRE2 compilation failed");
    }

    match_data = pcre2_match_data_create_from_pattern(re, NULL);
    if (!match_data) {
        pcre2_code_free(re);
        throw std::runtime_error("Failed to create match data");
    }

    int rc = pcre2_match(re, (PCRE2_SPTR)text.c_str(), text.length(), 0, 0,
                         match_data, NULL);

    std::string ext;
    if (rc >= 3) {
        ovector = pcre2_get_ovector_pointer(match_data);
        ext.assign(text.substr(ovector[4], ovector[5] - ovector[4]));
    }

    to_lowercase(ext);
    pcre2_code_free(re);
    pcre2_match_data_free(match_data);

    return ext;
}