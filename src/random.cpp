#include "random.hpp"

#include <random>
#include <stdexcept>
#include <unordered_set>

std::vector<int> Random::uniqueInts(int numSamples, int minValue,
                                    int maxValue) {
    if (numSamples > (maxValue - minValue + 1)) {
        throw std::invalid_argument(
            "number of samples exceeds the range of unique values");
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