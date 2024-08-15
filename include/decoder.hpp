#ifndef DECODER_HPP
#define DECODER_HPP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mpg123.h>
#include <opusfile.h>
#include <soxr.h>
#include <string>
#include <string_view>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <vector>

namespace fs = std::filesystem;

class SoxrResampler {
  public:
    SoxrResampler(double inputRate, double outputRate, int outChannels,
                  soxr_datatype_t inType, soxr_datatype_t outType, int quality);
    ~SoxrResampler();

    void process(const std::vector<int16_t> &audioBuffer, size_t inputLength,
                 std::vector<int16_t> &resampledBuffer, size_t outBufferSize,
                 size_t &resampledSize);

  private:
    soxr_t handle;
};

class BaseDecoder {
  public:
    virtual ~BaseDecoder() = default;
    virtual void decode() = 0;

  protected:
    BaseDecoder(const fs::path &filePath, const std::string_view &pipeName);
    void openPipe();
    void printDecodingProgress(std::chrono::seconds currentPosition,
                               const std::string &durStr);

    const fs::path filePath;
    const std::string pipeName;
    std::ofstream pipe;
};

class OpusDecoder : public BaseDecoder {
  public:
    OpusDecoder(const fs::path &filePath, const std::string_view &pipeName);
    ~OpusDecoder();
    void decode() override;

  private:
    OggOpusFile *opusFile = nullptr;

    void openOpusFile();
    void readAndWriteOpusData(opus_int64 totalSamples, double opusSampleRate,
                              const std::string &durStr);
};

class Mp3Decoder : public BaseDecoder {
  public:
    Mp3Decoder(const fs::path &filePath, const std::string_view &pipeName);
    ~Mp3Decoder();
    void decode() override;

  private:
    mpg123_handle *mpg123Handle = nullptr;

    long inSampleRate = 0;
    int inChannels = 0;
    int outChannels = 2;
    long outSampleRate = 48000;
    size_t outBufferSize = 0;
    std::vector<int16_t> audioBuffer;
    std::vector<int16_t> resampledBuffer;

    void initializeMpg123();
    void createMpg123Handle();
    void openMp3File();
    void getMp3Format(long &inSampleRate, int &inChannels, int &encoding);
    void setMp3Format(int outChannels, long outSampleRate, int encoding);
    std::chrono::seconds getMp3TotalDuration(long inSampleRate);
    void readResampleAndWriteMp3Data(std::chrono::seconds totalDuration);
    void processWriteData(bool resample, SoxrResampler &soxrResampler,
                          size_t bytesRead);
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
    std::unique_ptr<BaseDecoder> decoder;

    void printTag(const std::string_view name, const TagLib::String &value);
    void printAudioProperties(const TagLib::AudioProperties *properties);
    void printFileProperties(const TagLib::PropertyMap &fileProperties);
};

#endif // DECODER_HPP
