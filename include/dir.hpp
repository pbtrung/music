#ifndef DIR_HPP
#define DIR_HPP

#include <string>
#include <string_view>

class Dir {
  public:
    // Recursively deletes the directory and its contents at the given path.
    static void deleteDirectory(std::string_view path);

    // Creates a directory at the given path, including any missing parent
    // directories.
    static void createDirectory(std::string_view path);
};

#endif // DIR_HPP