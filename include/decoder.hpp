#ifndef DECODER_HPP
#define DECODER_HPP

#include <filesystem>
#include <sndfile.h>
#include <soxr.h>
#include <string>
#include <string_view>

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
    void process(const std::vector<short> &audioBuffer,
                 std::vector<short> &resampledBuffer,
                 size_t framesRead,
                 size_t *resampledSize);

  private:
    soxr_error_t error;
    soxr_t handle;
    soxr_io_spec_t iospec;
    soxr_quality_spec_t q_spec;
};

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