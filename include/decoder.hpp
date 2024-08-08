#ifndef DECODER_HPP
#define DECODER_HPP

#include <fstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

class Decoder {
  public:
    Decoder(const std::string &input_filename, const std::string &output_pipe);
    ~Decoder();

    void printMetadata() const;
    void decode();

  private:
    void init();
    void decodeFrame();
    void processFrame();
    void flushDecoder();

    std::string input_filename_;
    std::string output_pipe_;
    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *frame_ = nullptr;
    SwrContext *swr_ctx_ = nullptr;
    std::ofstream output_fp_;
    int stream_index_ = -1;
    int64_t duration_ = 0;
    std::string duration_str_;
};

#endif // DECODER_HPP