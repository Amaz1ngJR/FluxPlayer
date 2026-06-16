#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <string>

namespace FluxPlayer {

// 前向声明
class Player;

/**
 * 播放器命令基类
 *
 * 控制线程从队列取出命令后调用 execute()，在控制线程上下文执行。
 * 所有改变播放管线状态的操作（seek/pause/resume/stop/录制 start/stop/变速/
 * 画质切换）都封装为命令，由 UI 线程投递、控制线程串行执行，消除「UI 线程
 * 直接改内部状态」的并发脆弱性。
 */
struct PlayerCommand {
    virtual ~PlayerCommand() = default;

    /**
     * 在控制线程上下文执行命令
     * @param player 播放器实例（控制线程持有，命令通过它操作播放管线）
     */
    virtual void execute(Player& player) = 0;
};

/**
 * 命令队列：线程安全，支持任意线程投递、控制线程批量取走
 *
 * 设计要点：
 * - post() 非阻塞，临界区极短（move 指针）
 * - drain() 一次性取走全部待执行命令，锁外串行 execute
 * - 不在队列内做命令合并（连续 seek 等），由 Player::pumpCommands() 归并后执行
 */
class CommandQueue {
public:
    CommandQueue() = default;

    // 禁止拷贝
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    /**
     * 投递命令（任意线程，非阻塞）
     * @param cmd 命令智能指针（转移所有权）
     */
    void post(std::unique_ptr<PlayerCommand> cmd);

    /**
     * 取走全部待执行命令（仅控制线程调用）
     * @return 命令列表（可能为空）
     */
    std::vector<std::unique_ptr<PlayerCommand>> drain();

    /**
     * 查询当前待执行命令数（调试用，结果可能立即过时）
     */
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<PlayerCommand>> pending_;
};

// ===== 具体命令类型（后续阶段逐步补充）=====

/**
 * Seek 命令：跳转到指定时间
 * 在控制线程执行 seek：置精确跳转状态 + flush 队列 + 投递 demux 内部 seek 请求
 */
struct SeekCommand : PlayerCommand {
    double targetSec;

    explicit SeekCommand(double target) : targetSec(target) {}
    void execute(Player& player) override;
};

/**
 * 暂停命令
 */
struct PauseCommand : PlayerCommand {
    void execute(Player& player) override;
};

/**
 * 恢复播放命令
 */
struct ResumeCommand : PlayerCommand {
    void execute(Player& player) override;
};

/**
 * 停止播放命令
 */
struct StopCommand : PlayerCommand {
    void execute(Player& player) override;
};

/**
 * 设置播放速度命令
 */
struct SetSpeedCommand : PlayerCommand {
    double speed;

    explicit SetSpeedCommand(double s) : speed(s) {}
    void execute(Player& player) override;
};

/**
 * 画质切换命令
 * 切换画质：重新提取流、重开 demuxer、重启 worker，保持播放位置
 */
struct SwitchQualityCommand : PlayerCommand {
    std::string formatId;
    double seekTime;

    SwitchQualityCommand(const std::string& fid, double st)
        : formatId(fid), seekTime(st) {}
    void execute(Player& player) override;
};

/**
 * 开始录制命令（视频或音频）
 * 录制器创建/销毁归 demux 线程串行
 */
struct StartRecordingCommand : PlayerCommand {
    bool video;  ///< true=录像，false=录音

    explicit StartRecordingCommand(bool v) : video(v) {}
    void execute(Player& player) override;
};

/**
 * 停止录制命令
 */
struct StopRecordingCommand : PlayerCommand {
    bool video;

    explicit StopRecordingCommand(bool v) : video(v) {}
    void execute(Player& player) override;
};

} // namespace FluxPlayer
