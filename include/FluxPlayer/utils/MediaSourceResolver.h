#pragma once
#include "FluxPlayer/utils/MediaSource.h"

namespace FluxPlayer {
// Refreshes the chosen source without silently switching extraction backends
// or choosing a different quality. Work on a task-owned session snapshot.
class MediaSourceResolver {
public:
    static bool refresh(ExtractedStream& stream, std::string& error,
                        const std::atomic<bool>* cancelled = nullptr);
};
}
