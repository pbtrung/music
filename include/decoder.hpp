#ifndef DECODER_HPP
#define DECODER_HPP

#include <filesystem>
#include <string>
#include <string_view>

class Decoder {
  public:
    Decoder(const std::filesystem::path &filePath,
            std::string_view extension,
            std::string_view pipeName);
    void printMetadata();
    void decode();

  private:
    void decodeOpus();
    void decodeMp3();
    void printDecodingProgress(const std::chrono::seconds currentPosition,
                               const std::string &durStr);

    std::filesystem::path filePath;
    std::string extension;
    std::string pipeName;
};

#endif // DECODER_HPP