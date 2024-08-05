#ifndef DECODER_HPP
#define DECODER_HPP

#include <string>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

class decoder {
public:
    decoder(const std::string &input_filename, const std::string &output_pipe);
    ~decoder();

    void print_metadata() const;
    void decode();

private:
    void init();
    void cleanup();

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
};

#endif // DECODER_HPP
