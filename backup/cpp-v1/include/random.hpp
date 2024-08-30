#ifndef RANDOM_HPP
#define RANDOM_HPP

#include <string>
#include <string_view>
#include <vector>

class Random {
  public:
    // Generates a vector of unique random integers within a specified range.
    static std::vector<int> uniqueInts(int numSamples, int minValue,
                                       int maxValue);

    // Generates a random alphanumeric string of a specified length.
    static std::string alphanumericString(int length);
};

#endif // RANDOM_HPP