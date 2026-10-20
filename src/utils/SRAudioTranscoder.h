#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
struct AVStream;
struct AVFormatContext;
struct AVPacket;
namespace FluxPlayer {
/// 私有 AAC 转码器，单 Worker 调用；有界 FIFO，沿用共同媒体原点。
class SRAudioTranscoder {
public:
    SRAudioTranscoder(AVStream* source, AVFormatContext* output, int64_t originUs,
                      const std::atomic<bool>& cancelled);
    ~SRAudioTranscoder();
    void push(const AVPacket* packet);
    void finish();
    std::string repairSummary() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
