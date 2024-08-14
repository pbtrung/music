#ifndef DECODER_HPP
#define DECODER_HPP

#include <filesystem>
#include <functional>
#include <memory>
#include <mpg123.h>
#include <opusfile.h>
#include <soxr.h>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

class SoxrResampler {
  public:
    SoxrResampler(double inputRate, double outputRate, int channels,
                  soxr_datatype_t in_type, soxr_datatype_t out_type,
                  int quality);
    ~SoxrResampler();

    void process(const std::vector<int16_t> &audioBuffer, size_t inputLength,
                 std::vector<int16_t> &resampledBuffer, size_t outBufferSize,
                 size_t &resampledSize);

  private:
    soxr_t handle;
};

class Decoder {
  public:
    Decoder(const fs::path &filePath, const std::string_view &extension,
            const std::string_view &pipeName);

    void printMetadata();
    void decode();

  private:
    const fs::path filePath;
    const std::string extension;
    const std::string pipeName;

    std::ofstream openPipe();
    void printDecodingProgress(std::chrono::seconds currentPosition,
                               const std::string &durStr);

    void decodeOpus();
    std::unique_ptr<OggOpusFile, decltype(&op_free)> openOpusFile();
    void readAndWriteOpusData(OggOpusFile *of, std::ofstream &pipe,
                              opus_int64 totalSamples, double opusSampleRate,
                              const std::string &durStr);

    void decodeMp3();
    void initializeMpg123();
    std::unique_ptr<mpg123_handle, std::function<void(mpg123_handle *)>>
    createMpg123Handle();
    void openMp3File(mpg123_handle *mh);
    void getMp3Format(mpg123_handle *mh, long &inSampleRate, int &inChannels,
                      int &encoding);
    void setMp3Format(mpg123_handle *mh, int outChannels, long outSampleRate,
                      int encoding);
    std::chrono::seconds getMp3TotalDuration(mpg123_handle *mh,
                                             long inSampleRate);
    void readResampleAndWriteMp3Data(mpg123_handle *mh, std::ofstream &pipe,
                                     long inSampleRate, int inChannels,
                                     std::chrono::seconds totalDuration,
                                     SoxrResampler &soxrResampler);
};

#endif // DECODER_HPP
