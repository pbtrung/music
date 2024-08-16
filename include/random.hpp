#ifndef RANDOM_HPP
#define RANDOM_HPP

#include <string>
#include <string_view>
#include <vector>

class Random {
  public:
    static std::vector<int> uniqueInts(int numSamples, int minValue,
                                       int maxValue);
    static std::string alphanumericString(int length);
};

#endif // RANDOM_HPP