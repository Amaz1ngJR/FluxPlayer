#pragma once

#include "FluxPlayer/utils/MediaSource.h"

namespace FluxPlayer {

// Parse actual master-playlist variants into the player's existing quality
// options. Network methods belong on an opening/quality worker, never the UI.
class HlsStreamResolver {
public:
    static std::vector<QualityOption> parseMaster(const std::string& playlist,
                                                 const std::string& masterUrl);
    static bool populate(ExtractedStream& stream, std::string& error,
                         const std::atomic<bool>* cancelled = nullptr);
    static bool select(ExtractedStream& stream, const std::string& formatId,
                       std::string& error, const std::atomic<bool>* cancelled = nullptr);
};

} // namespace FluxPlayer
