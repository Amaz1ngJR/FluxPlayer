#include "FluxPlayer/core/PlayerCommand.h"
#include "FluxPlayer/core/Player.h"

namespace FluxPlayer {

// CommandQueue

void CommandQueue::post(std::unique_ptr<PlayerCommand> cmd) {
    if (!cmd) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back(std::move(cmd));
}

std::vector<std::unique_ptr<PlayerCommand>> CommandQueue::drain() {
    std::vector<std::unique_ptr<PlayerCommand>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    // swap 取走全部，锁外执行，避免持锁跑重活
    out.swap(pending_);
    return out;
}

size_t CommandQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

// 具体命令 execute()
//
// 命令 execute() 委托给 Player 的内部执行方法（*Internal）。
// UI 侧仍走 Player 公开方法（seek/pause/...）；公开方法在后续阶段会改为「投递命令」，
// 命令 execute() 调用内部方法真正落地，从而消除「公开方法 → 投递 → execute → 公开方法」
// 的递归（公开方法投递命令、命令 execute 调用 *Internal 真正落地）。

void SeekCommand::execute(Player& player) {
    player.seekInternal(targetSec);
}

void PauseCommand::execute(Player& player) {
    player.pauseInternal();
}

void ResumeCommand::execute(Player& player) {
    player.resumeInternal();
}

void StopCommand::execute(Player& player) {
    player.stopInternal();
}

void SetSpeedCommand::execute(Player& player) {
    player.setPlaybackSpeedInternal(speed);
}

void SwitchQualityCommand::execute(Player& player) {
    player.switchQualityInternal(formatId, seekTime);
}

void StartRecordingCommand::execute(Player& player) {
    if (video) {
        player.startVideoRecordingInternal();
    } else {
        player.startAudioRecordingInternal();
    }
}

void StopRecordingCommand::execute(Player& player) {
    if (video) {
        player.stopVideoRecordingInternal();
    } else {
        player.stopAudioRecordingInternal();
    }
}

} // namespace FluxPlayer
