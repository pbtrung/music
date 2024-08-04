#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <vector>

class utils {
  public:
    // Static methods for random number generation
    static void to_lowercase(std::string &str);
    static std::string get_extension(const std::string &text);
    static std::string get_time(double seconds);
};

#endif // UTILS_HPP
