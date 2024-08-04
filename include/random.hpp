#ifndef RANDOM_HPP
#define RANDOM_HPP

#include <string>
#include <vector>

class rng {
  public:
    // Static methods for random number generation
    static std::vector<int> random_ints(int num_samples, int min_value,
                                        int max_value);
    static std::string random_string(int length);
};

#endif // RANDOM_HPP
