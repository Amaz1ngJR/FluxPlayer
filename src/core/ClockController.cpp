#include "FluxPlayer/core/ClockController.h"
#include "FluxPlayer/core/AVSync.h"
#include "FluxPlayer/core/TimeUtils.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

ClockController::ClockController() {
    avSync_ = std::make_unique<AVSync>(ClockType::EXTERNAL_CLOCK);
    LOG_INFO("ClockController created");
}

ClockController::~ClockController() {
    LOG_INFO("ClockController destroyed");
}

// Seek 精确跳转状态管理

void ClockController::startSeekToTarget(double targetPTS) {
    decodingToTarget_.store(true);
    decodeTargetPTS_.store(targetPTS);
    seekTargetStartNs_.store(steadyNowNs());
    LOG_INFO("ClockController: seek to target started, targetPTS=" + std::to_string(targetPTS));
}

bool ClockController::isSeekTimedOut(double timeoutSec) const {
    // 计时窗口独立于视频的精确解码标志：视频到达目标后音频仍可能缺包。
    const int64_t start = seekTargetStartNs_.load();
    if (start == 0) return false;
    double elapsed = static_cast<double>(steadyNowNs() - start) / 1e9;
    return elapsed > timeoutSec;
}

void ClockController::resetSeekTimer() {
    seekTargetStartNs_.store(steadyNowNs());
}

void ClockController::resetSeekState() {
    decodingToTarget_.store(false);
    decodeTargetPTS_.store(0.0);
    seekTargetStartNs_.store(0);
    LOG_INFO("ClockController: seek state reset");
}

} // namespace FluxPlayer
