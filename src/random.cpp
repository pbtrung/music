#include "random.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <random>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <unordered_set>

std::vector<int> Random::uniqueInts(int numSamples, int minValue,
                                    int maxValue) {
    std::shared_ptr<spdlog::logger> logger = spdlog::get("logger");
    if (numSamples > (maxValue - minValue + 1)) {
        SPDLOG_LOGGER_ERROR(
            logger, "Number of samples exceeds the range of unique values");
        throw std::invalid_argument("");
    }

    std::unordered_set<int> uniqueSet;
    std::vector<int> result;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(minValue, maxValue);

    while (result.size() < numSamples) {
        int value = dis(gen);
        if (uniqueSet.insert(value).second) {
            result.push_back(value);
        }
    }

    return result;
}

std::string Random::alphanumericString(int length) {
    static constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, alphabet.size() - 1);

    std::string result(length, '\0');
    std::generate(result.begin(), result.end(),
                  [&]() { return alphabet[dis(gen)]; });
    return result;
}