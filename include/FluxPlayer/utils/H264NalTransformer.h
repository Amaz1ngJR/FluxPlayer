#pragma once

#include <memory>
#include <string>

struct AVPacket;

namespace FluxPlayer {

enum class CctvProtectionMode { None, ClassicLive, H5E };

// Stateful H.264 packet transform. Owned and called only by the Demuxer I/O
// thread; retains enable/layout state across packets, resets on seek/reopen.
class H264NalTransformer {
public:
    explicit H264NalTransformer(CctvProtectionMode mode);
    ~H264NalTransformer();
    H264NalTransformer(const H264NalTransformer&) = delete;
    H264NalTransformer& operator=(const H264NalTransformer&) = delete;

    static CctvProtectionMode detect(const std::string& mediaUrl,
                                     const std::string& pageUrl = "",
                                     bool observedCctvProtection = false);
    bool process(AVPacket* packet, std::string& error);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace FluxPlayer
