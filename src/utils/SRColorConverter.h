#pragma once
#include <memory>
#include <cstdint>
#include <string>
extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
}
namespace FluxPlayer {
/// Per-export color normalization. SDR borrows pixels; HDR/WCG uses a cached graph.
class SRColorConverter {
public:
    SRColorConverter();
    ~SRColorConverter();
    const AVFrame* prepare(const AVFrame* input, const AVCodecParameters* params, AVRational timeBase);
    std::string description() const;
    uint64_t scratchBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
