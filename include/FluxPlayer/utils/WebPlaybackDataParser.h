#pragma once
#include "FluxPlayer/utils/MediaSource.h"
namespace FluxPlayer {
// Parse playback data already supplied to the logged-in web page.
bool parseWebPlaybackData(const std::string& message, ExtractedStream& stream);
}
