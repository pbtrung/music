#ifndef DIR_HPP
#define DIR_HPP

#include <string>
#include <string_view>

class Dir {
  public:
    static void deleteDirectory(std::string_view path);
    static void createDirectory(std::string_view path);
};

#endif // DIR_HPP