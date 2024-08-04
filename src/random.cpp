#include "random.hpp"
#include <random>
#include <stdexcept>
#include <unordered_set>

std::vector<int> rng::random_ints(int num_samples, int min_value,
                                  int max_value) {
    if (num_samples > (max_value - min_value + 1)) {
        throw std::invalid_argument(
            "Number of samples exceeds the range of unique values");
    }

    std::random_device rd;  // Obtain a seed from a true random number generator
    std::mt19937 gen(rd()); // Standard mersenne_twister_engine seeded with rd()
    std::uniform_int_distribution<> dis(min_value, max_value);

    std::vector<int> unique_random_ints;
    std::unordered_set<int> seen;

    while (unique_random_ints.size() < num_samples) {
        int num = dis(gen);
        if (!seen.count(num)) {
            unique_random_ints.push_back(num);
            seen.insert(num);
        }
    }

    return unique_random_ints;
}

// Generate a random string of specified length
std::string rng::random_string(int length) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(
        0, 61); // 26 lowercase + 26 uppercase + 10 digits = 62

    const std::string alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result(length, '\0');

    for (char &c : result) {
        c = alphabet[dis(gen)];
    }

    return result;
}
