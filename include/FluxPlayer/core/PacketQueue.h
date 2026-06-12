/**
 * @file PacketQueue.h
 * @brief 线程安全的压缩数据包队列，对标 ffplay PacketQueue
 *
 * 用于解耦 demux 线程（生产者）与 video/audio 解码线程（消费者）：
 * - demux 线程读到 AVPacket 后按流分发，put() 进对应队列（非阻塞追加）
 * - 解码线程 get() 取包解码；队列空时可阻塞等待，避免空转
 * - serial 序号标记 flush 边界：seek/重连后队列 flush 自增 serial，
 *   解码线程发现 serial 变化即知道应 flush 自身解码器与帧队列
 * - 维护字节数与时长统计，供 demux 线程做背压（防止单路无限缓冲）
 *
 * 线程模型：单生产者（demux）+ 单消费者（对应解码线程）+ 任意线程可
 * flush/abort/start（受内部 mutex 保护）。
 */

#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

namespace FluxPlayer {

/**
 * @brief 压缩数据包队列（链表实现，仿 ffplay PacketQueue）
 *
 * 每个入队的包记录入队时的 serial；flush() 时 serial 自增，旧 serial 的包被
 * 清空，新读入的包带上新的 serial。消费者通过 serial 变化识别 seek/flush 边界。
 */
class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    /**
     * @brief 设置该队列对应流的时间基准，用于 duration() 秒数换算
     *
     * 未设置（默认 0/1）时 duration() 返回 0，调用方应退化为字节/包数门槛。
     * @param tb 流的 time_base（来自 AVStream::time_base）
     */
    void setTimeBase(AVRational tb);

    // ==================== 生产者接口（demux 线程调用） ====================

    /**
     * @brief 入队一个数据包（非阻塞追加）
     *
     * 通过 av_packet_move_ref 转移 pkt 的数据所有权到队列内部节点，
     * 调用方的 pkt 在返回后变为空包（无需再 unref，可直接复用读下一个包）。
     * @param pkt 待入队的包（数据所有权被转移）
     * @return 成功返回 true；abort 状态或内存分配失败返回 false
     */
    bool put(AVPacket* pkt);

    /**
     * @brief 入队一个 EOF 标记包（data=NULL, size=0）
     *
     * 解码线程取到该包后向解码器送入 drain 信号，取尽残留帧后结束。
     * @param streamIndex 该 EOF 标记所属流索引（写入包的 stream_index）
     */
    void putNullPacket(int streamIndex);

    // ==================== 消费者接口（解码线程调用） ====================

    /**
     * @brief 取出一个数据包
     *
     * 通过 av_packet_move_ref 把队列节点的数据转移到调用方的 pkt。
     * @param pkt    接收数据的包（调用方预先 av_packet_alloc，可复用）
     * @param block  队列空时是否阻塞等待（true 阻塞直到有包或 abort）
     * @param serial 出参：返回该包所属的 serial 序号（可为 nullptr）
     * @return 1=成功取到包；0=abort（应退出线程）；-1=非阻塞且队列空
     */
    int get(AVPacket* pkt, bool block, int* serial);

    // ==================== 控制接口 ====================

    /**
     * @brief 清空队列所有包，serial 自增
     *
     * 用于 seek / DASH 重启 / 直播重连：丢弃旧序号的全部缓冲包。
     * 唤醒可能阻塞在 get() 的消费者，使其重新检查状态。
     */
    void flush();

    /**
     * @brief 终止队列，唤醒所有等待者
     *
     * 设置 abort 标志后，get() 立即返回 0。用于 stop/quit。
     */
    void abort();

    /**
     * @brief 重置 abort 状态并使队列可用，serial 自增
     *
     * 用于循环播放 / 重新播放时重新启用队列。
     */
    void start();

    // ==================== 状态查询 ====================

    /** @brief 当前队列中的包数 */
    int size() const;

    /** @brief 当前队列总字节数（含包数据与节点结构开销，用于背压） */
    int64_t byteSize() const;

    /**
     * @brief 当前队列缓存的时长（秒）
     *
     * 由队列内各包 duration 累加 × time_base 得出。未设置 time_base 时返回 0。
     * 字节数对高码率视频/低码率音频区分度差，时长是更稳定的背压维度。
     */
    double duration() const;

    /** @brief 当前 serial 序号 */
    int serial() const;

    /** @brief 是否处于 abort 状态 */
    bool isAbort() const { return abort_.load(); }

private:
    /// 队列节点：持有一个包及其入队时的 serial
    struct Node {
        AVPacket* pkt;   ///< 节点拥有的包（av_packet_alloc 分配）
        int serial;      ///< 入队时的 serial 序号
        Node* next;      ///< 后继节点
    };

    /// 内部清空实现（调用方须已持有 mutex_）
    void flushLocked();

    Node* first_;            ///< 队首（消费端）
    Node* last_;             ///< 队尾（生产端）
    int nbPackets_;          ///< 当前包数
    int64_t byteSize_;       ///< 当前总字节数
    int64_t durationTs_;     ///< 当前总时长（time_base 单位，未换算）
    int serial_;             ///< 当前 serial 序号
    AVRational timeBase_;    ///< 流时间基准，用于 duration 秒数换算

    mutable std::mutex mutex_;
    std::condition_variable cond_;    ///< 消费者等待：有包入队或 abort 时唤醒
    std::atomic<bool> abort_{false};  ///< 终止标志
};

} // namespace FluxPlayer
