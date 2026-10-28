#pragma once
#include "FluxPlayer/utils/MediaSource.h"
#include <condition_variable>
#include <mutex>
#include <chrono>

namespace FluxPlayer {

// Shared by an I/O worker and a main-thread browser. The browser never retains
// the caller's cancellation pointer; abandonment survives the worker returning.
struct BrowserRefreshRequest {
    explicit BrowserRefreshRequest(const ExtractedStream& value) : previous(value) {}
    const ExtractedStream previous;
    std::atomic<bool> abandoned{false};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool match(const ExtractedStream& candidate, ExtractedStream& selected) const;
    void finish(const ExtractedStream& value, const std::string& error);
    bool wait(ExtractedStream& out, std::string& error, const std::atomic<bool>* cancelled);
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    ExtractedStream result;
    std::string failure;
};

class BrowserRefresh {
public:
    static void initialize(); // Call on the UI thread before a capture session.
    static bool refresh(const ExtractedStream& previous, ExtractedStream& out,
                        std::string& error, const std::atomic<bool>* cancelled);
};
}
