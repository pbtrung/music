#ifndef DIR_HPP
#define DIR_HPP

#include <string>

class dir {
  public:
    // Deletes the directory at the given path
    static void delete_directory(const std::string &path);

    // Creates a directory at the given path
    static void create_directory(const std::string &path);
};

#endif // DIR_HPP
