#ifndef DECODER_HPP
#define DECODER_HPP

#include <filesystem>
#include <string>
#include <stdexcept>

class decoder {
  public:
    decoder(const std::filesystem::path &file_path, const std::string &ext,
            const std::string &pipe_name);
    void print_metadata();
    void decode();

  private:
    std::filesystem::path file_path;
    std::string ext;
    std::string pipe_name;
};

#endif // DECODER_HPP