#include "utils.hpp"
#include <algorithm>
#include <chrono>
#include <fmt/core.h>
#include <fstream>
#include <spdlog/spdlog.h>
#include <stdexcept>

std::array<uint8_t, BLAKE2S_OUTBYTES>
Utils::getBlake2Hash(const std::string &filename) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        SPDLOG_LOGGER_ERROR(logger, "Error: Failed to open file: {}", filename);
        throw std::runtime_error("");
    }

    blake2s_state state;
    blake2s_init(&state, BLAKE2S_OUTBYTES);

    const size_t buffer_size = 8192;
    std::vector<char> buffer(buffer_size);
    while (file.read(buffer.data(), buffer_size)) {
        blake2s_update(&state, reinterpret_cast<uint8_t *>(buffer.data()),
                       file.gcount());
    }

    std::array<uint8_t, BLAKE2S_OUTBYTES> hashArray;
    blake2s_final(&state, hashArray.data(), BLAKE2S_OUTBYTES);

    return hashArray;
}

Utils::Pcre2CodePtr Utils::compilePattern(std::string_view pattern) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                   PCRE2_ZERO_TERMINATED, PCRE2_CASELESS,
                                   &errcode, &erroffset, nullptr);

    if (!re) {
        SPDLOG_LOGGER_ERROR(logger, "PCRE2 compilation failed at offset {}",
                            erroffset);
        throw std::runtime_error("");
    }
    return Pcre2CodePtr(re, pcre2_code_free);
}

Utils::Pcre2MatchDataPtr Utils::createMatchData(const pcre2_code *pattern) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    pcre2_match_data *match_data =
        pcre2_match_data_create_from_pattern(pattern, nullptr);
    if (!match_data) {
        SPDLOG_LOGGER_ERROR(logger, "Failed to create match data");
        throw std::runtime_error("");
    }
    return Pcre2MatchDataPtr(match_data, pcre2_match_data_free);
}

void Utils::toLowercase(std::string &str) {
    std::ranges::transform(str, str.begin(),
                           [](unsigned char c) { return std::tolower(c); });
}

std::string Utils::getExtension(std::string_view text) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
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
        SPDLOG_LOGGER_ERROR(logger, "Failed to find extension from {}", text);
        throw std::runtime_error("");
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
