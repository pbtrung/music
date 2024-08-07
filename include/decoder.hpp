#ifndef DECODER_HPP
#define DECODER_HPP

#include <filesystem>
#include <sndfile.h>
#include <string>
#include <string_view>

class Decoder {
  public:
    Decoder(const std::filesystem::path &filePath, std::string_view pipeName);
    void printMetadata();
    void decode();

  private:
    void decodeSndFile();

    std::filesystem::path filePath;
    std::string pipeName;
};

#endif // DECODER_HPP