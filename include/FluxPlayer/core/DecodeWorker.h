#pragma once

#include <memory>
#include <thread>

namespace FluxPlayer {

class Player;
class Frame;
class FrameQueue;
class PacketQueue;

/**
 * 解码流类型
 */
enum class StreamKind { Video, Audio };

/**
 * 解码工作线程封装
 *
 * 职责：持有 video 或 audio 解码线程，执行解码循环。
 * Video/Audio 解码逻辑通过 kind 区分，辅助方法（enqueue/normalize）内聚于本类。
 *
 * 线程安全约束：
 * - start/join 仅在控制线程调用
 * - run() 在解码线程执行
 */
class DecodeWorker {
public:
    DecodeWorker(Player* player, StreamKind kind);
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    void start();
    void join();

    bool isJoinable() const { return thread_ && thread_->joinable(); }
    StreamKind kind() const { return kind_; }

private:
    void run();           // 分发到 runVideo/runAudio
    void runVideo();      // 视频解码循环
    void runAudio();      // 音频解码循环

    // 解码辅助方法（从 Player 迁移）
    void checkPrebufferComplete();
    bool normalizeVideoPTS(Frame& rawFrame);
    bool normalizeAudioPTS(Frame& rawFrame);
    bool enqueueVideoFrame(Frame& rawFrame, int serial);
    bool enqueueAudioFrame(Frame& rawFrame, int serial, bool* enqueued);
    Frame* waitWritableSlot(FrameQueue* frameQueue, PacketQueue* pktQueue, int serial);

    Player* player_;
    StreamKind kind_;
    std::unique_ptr<std::thread> thread_;
};

} // namespace FluxPlayer
