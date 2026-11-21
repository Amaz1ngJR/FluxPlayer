#pragma once
#include <memory>
#include <utility>
#include <string>
struct AVFrame;
namespace FluxPlayer {
// Owns its worker/model/resampler. No inference or resampling on audio callbacks.
class RealtimeSubtitles {
public:
    RealtimeSubtitles(std::string model, std::string language, bool gpu, std::string hints = {});
    ~RealtimeSubtitles();
    void reset(int serial);
    void push(const AVFrame* frame, double pts, int serial);
    void finish(int serial);
    void requestStop();
    std::pair<std::string, std::string> parts(double mediaTime, int serial, std::string* correction = nullptr);
    std::string text(double mediaTime, int serial);
    std::string status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
