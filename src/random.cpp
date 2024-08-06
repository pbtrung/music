#include "random.hpp"

#include <algorithm> // for std::shuffle
#include <random>
#include <stdexcept>

std::vector<int> Random::uniqueInts(int numSamples, int minValue,
                                    int maxValue) {
    if (numSamples > (maxValue - minValue + 1)) {
        throw std::invalid_argument(
            "number of samples exceeds the range of unique values");
    }

    std::vector<int> result(maxValue - minValue + 1);
    std::iota(result.begin(), result.end(), minValue);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(result.begin(), result.end(), gen);

    return std::vector<int>(result.begin(), result.begin() + numSamples);
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