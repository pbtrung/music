#ifndef DECODER_HPP
#define DECODER_HPP

#include <filesystem>
#include <mpg123.h>
#include <soxr.h>
#include <string>
#include <string_view>
#include <vector>

class SoxrHandle {
  public:
    SoxrHandle(double inputRate,
               double outputRate,
               int channels,
               const soxr_datatype_t &in_type,
               const soxr_datatype_t &out_type,
               int quality);
    ~SoxrHandle();
    soxr_t get() const;
    void process(const std::vector<unsigned char> &audioBuffer,
                 std::vector<unsigned char> &resampledBuffer,
                 size_t bytesRead,
                 size_t *resampledSize);

  private:
    soxr_error_t error;
    soxr_t handle;
    soxr_io_spec_t iospec;
    soxr_quality_spec_t q_spec;
};

class Decoder {
  public:
    Decoder(const std::filesystem::path &filePath,
            std::string_view extension,
            std::string_view pipeName);

    void printMetadata();
    void decode();

  private:
    std::filesystem::path filePath;
    std::string extension;
    std::string pipeName;

    void decodeMp3();
    void decodeOpus();
    void decodeMp3File(
        std::unique_ptr<mpg123_handle, decltype(&mpg123_delete)> &mhPtr,
        SoxrHandle &soxrHandle,
        std::ofstream &pipe,
        long sampleRate,
        int channels);
    void writeToPipe(std::ofstream &pipe,
                     const std::vector<unsigned char> &buffer,
                     size_t size);
    void printDecodingProgress(const std::chrono::seconds currentPosition,
                               const std::string &durStr);
    void configureMpg123(mpg123_handle *mh,
                         long &sampleRate,
                         int &channels,
                         int &encoding);
};

#endif // DECODER_HPP