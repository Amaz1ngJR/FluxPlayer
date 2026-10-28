/**
 * @file Demuxer.h
 * @brief 媒体文件解复用器，负责打开媒体文件、分离音视频流、读取数据包
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <cstdint>
#include "FluxPlayer/utils/H264NalTransformer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace FluxPlayer {

/**
 * @brief 媒体解复用器，基于 FFmpeg libavformat
 *
 * 负责打开媒体文件（本地文件或网络流），解析流信息，
 * 查找音视频流，并按包（AVPacket）读取压缩数据供解码器使用。
 * 支持本地文件（MP4/MKV/AVI等）和网络流（RTSP/RTMP/HTTP等）。
 */
class Demuxer {
public:
    /** @brief stop 标志须覆盖 Demuxer 生命周期；只发布原子意图，不跨线程 close。 */
    explicit Demuxer(const std::atomic<bool>* externalStop = nullptr);
    ~Demuxer();
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    enum class IoResult { Ok, Cancelled, Superseded, ControlWake, TimedOut, EndOfFile, Again, Error };
    /** @brief 并发安全地取消；取消在 close/open 之间保持，不自动复活该对象。 */
    void requestCancel() { cancelled_.store(true, std::memory_order_release); }
    bool isCancelled() const;
    /** 仅在无 I/O/协议回调运行时由所有者更换停止标志，目标寿命须覆盖输入。 */
    void bindStopFlag(const std::atomic<bool>* stop) { externalStop_ = stop; }
    /** @brief 仅由 I/O 所属线程读取最近操作结果。 */
    IoResult lastIoResult() const { return lastIoResult_; }

    /**
     * @brief 打开媒体文件或网络流
     *
     * 自动检测格式，查找音视频流，打印媒体信息。
     * 对网络流会自动配置 RTSP/TCP 传输、超时等参数。
     * @param filename 文件路径或流 URL（支持 rtsp://, rtmp://, http:// 等）
     * @return 成功返回 true，失败返回 false
     */
    bool open(const std::string& filename);

    /**
     * @brief 打开网页提取的流（带 headers 和已知时长）
     * @param filename      流 URL 或 "pipe:N"
     * @param httpHeaders   HTTP headers（"Key: Value\r\n" 格式），空则不注入
     * @param knownDuration 已知时长（秒），>0 时手动注入 duration（解决 pipe 流被误判为直播流）
     * @param httpCookies   FFmpeg Cookie jar，保留 domain/path，供分片请求和重连复用
     */
    bool open(const std::string& filename,
              const std::string& httpHeaders,
              double knownDuration = 0.0,
              const std::string& httpCookies = "",
              CctvProtectionMode cctvProtection = CctvProtectionMode::None);

    /**
     * @brief 打开输入但不调用 avformat_find_stream_info。
     *
     * 仅用于 DashMerger 生成的自描述 Matroska pipe：Tracks 已含完整 codecpar，跳过探测可
     * 避免等待远端媒体包。若头信息不完整会显式失败，不影响通用 open() 的兼容路径。
     */
    bool openSelfDescribingPipe(const std::string& filename, double knownDuration = 0.0);

    /** @brief 关闭文件并释放所有资源 */
    void close();

    /**
     * @brief 从文件读取下一个压缩数据包
     * @param packet 用于接收数据的 AVPacket 指针（调用者需在使用后 av_packet_unref）
     * @return 成功返回 true，文件结束或出错返回 false
     */
    // 可选 generation 须在本次调用结束前有效；缺省仍响应 externalStop_。
    bool readPacket(AVPacket* packet, const std::atomic<uint64_t>* generation = nullptr,
                    uint64_t expectedGeneration = 0, const std::atomic<bool>* controlWake = nullptr);

    /// 最近一次 readPacket 的 FFmpeg 返回值，用于区分 EAGAIN、EOF 和永久错误。
    int getLastReadError() const { return m_lastReadError; }

    // ==================== 流索引 ====================

    /**
     * @brief 获取视频流索引
     * @return 视频流在容器中的索引，未找到视频流时返回 -1
     */
    int getVideoStreamIndex() const { return m_videoStreamIndex; }

    /**
     * @brief 获取音频流索引
     * @return 音频流在容器中的索引，未找到音频流时返回 -1
     */
    int getAudioStreamIndex() const { return m_audioStreamIndex; }

    /**
     * @brief 获取字幕流索引
     *
     * 阶段一仅识别第一条字幕流；多语言字幕的轨道选择留待阶段二。
     * @return 字幕流索引，未找到字幕流时返回 -1
     */
    int getSubtitleStreamIndex() const { return m_subtitleStreamIndex; }

    // ==================== 流和编解码器信息 ====================

    /**
     * @brief 获取视频流对象
     * @return AVStream 指针，无视频流时返回 nullptr
     */
    AVStream* getVideoStream() const;

    /**
     * @brief 获取音频流对象
     * @return AVStream 指针，无音频流时返回 nullptr
     */
    AVStream* getAudioStream() const;

    /**
     * @brief 获取视频编解码器参数（用于初始化 VideoDecoder）
     * @return AVCodecParameters 指针，无视频流时返回 nullptr
     */
    AVCodecParameters* getVideoCodecParams() const;

    /**
     * @brief 获取音频编解码器参数（用于初始化 AudioDecoder）
     * @return AVCodecParameters 指针，无音频流时返回 nullptr
     */
    AVCodecParameters* getAudioCodecParams() const;

    /**
     * @brief 获取字幕流对象
     * @return AVStream 指针，无字幕流时返回 nullptr
     */
    AVStream* getSubtitleStream() const;

    /**
     * @brief 获取字幕编解码器参数（用于初始化 SubtitleDecoder）
     * @return AVCodecParameters 指针，无字幕流时返回 nullptr
     */
    AVCodecParameters* getSubtitleCodecParams() const;

    // ==================== 媒体信息 ====================

    /**
     * @brief 获取媒体总时长
     * @return 时长（微秒），对于实时流可能无效
     */
    int64_t getDuration() const;

    /** @brief 获取视频宽度（像素），无视频流返回 0 */
    int getWidth() const;

    /** @brief 获取视频高度（像素），无视频流返回 0 */
    int getHeight() const;

    /** @brief 获取视频平均帧率（fps），无视频流返回 0.0 */
    double getFrameRate() const;

    /** @brief 获取媒体比特率（bps），无效时返回 0 */
    int getBitrate() const;

    // ==================== Seek 操作 ====================

    /**
     * @brief 跳转到指定时间位置
     *
     * 使用 avformat_seek_file 进行精确跳转。
     * 跳转后需刷新解码器缓冲区（调用 decoder.flush()）。
     * @param timestamp 目标时间戳（微秒）
     * @return 成功返回 true，失败返回 false
     */
    // generation 必须在本次同步调用及协议回调结束前有效；仅 I/O 所属线程调用。
    bool seek(int64_t timestamp, const std::atomic<uint64_t>* generation = nullptr,
              uint64_t expectedGeneration = 0);

    /**
     * @brief 检测是否为实时流（RTSP/RTMP/RTP 等）
     * @return true 表示实时流，false 表示本地文件或可点播流
     */
    bool isLiveStream() const;

    /**
     * @brief 获取底层 FFmpeg 格式上下文
     * @return AVFormatContext 指针
     */
    AVFormatContext* getFormatContext() { return m_formatCtx; }

private:
    // opaque 指向不可移动的 Demuxer；回调不分配、不记录日志、不获取业务锁。
    static int interruptCallback(void* opaque);
    bool prepareInput();
    bool preparePipeInput(const std::string& filename);
    static int readPipe(void* opaque, uint8_t* buffer, int size);
    void* pipeHandle_ = nullptr; ///< Windows HANDLE，仅 I/O 所属线程创建/关闭。
    int pipeFd_ = -1; ///< POSIX 读端所有权由 DashMerger 转入。
    AVIOContext* pipeIo_ = nullptr; ///< 自定义可取消 AVIO，close_input 后由所有者回收。
    bool beginIo(int timeoutSeconds);
    int finishIo(int result);
    std::atomic<const std::atomic<bool>*> externalStop_;
    std::atomic<bool> cancelled_{false};
    std::atomic<int64_t> deadlineNs_{0}; ///< 回调可能由协议内部线程读取，按原子快照发布。
    bool isSuperseded() const;
    bool isControlWake() const;
    std::atomic<const std::atomic<bool>*> controlWake_{nullptr};
    std::atomic<const std::atomic<uint64_t>*> seekGeneration_{nullptr};
    std::atomic<uint64_t> expectedSeekGeneration_{0};
    IoResult lastIoResult_ = IoResult::Ok;

    /**
     * @brief 为指定路由打开输入并探测流。
     * @param filename 输入 URL/路径
     * @param httpHeaders 附加 HTTP headers
     * @param knownDuration 已知时长
     * @param useConfiguredProxy 是否注入配置代理
     */
    bool openInternal(const std::string& filename,
                      const std::string& httpHeaders,
                      double knownDuration,
                      bool useConfiguredProxy,
                      const std::string& httpCookies,
                      CctvProtectionMode cctvProtection);

    /**
     * @brief 为网络流 URL 配置 FFmpeg 打开选项
     *
     * 按协议类型（HTTP/HLS、RTSP、RTMP）分别设置重连、超时、缓冲区等参数。
     * 本地文件直接返回空字典。
     * @param filename URL 或文件路径
     * @return AVDictionary 指针（由调用方负责 av_dict_free）
     */
    AVDictionary* configureNetworkOptions(const std::string& filename,
                                          bool useConfiguredProxy = true) const;

    /**
     * @brief 遍历 m_formatCtx->streams，填充视频/音频/字幕流索引
     *
     * 每类只选第一条。须在 avformat_find_stream_info 成功后调用。
     * @return 至少找到一条音频或视频流时返回 true
     */
    bool findStreams();

    /**
     * @brief 打印打开的媒体文件概要（格式、时长、码率、分辨率、采样率等）
     * @param filename 打开的文件或 URL，用于日志
     */
    void logMediaInfo(const std::string& filename) const;

    int m_lastReadError = 0;
    std::unique_ptr<H264NalTransformer> cctvDecoder_;
    AVFormatContext* m_formatCtx;    ///< FFmpeg 格式上下文，管理容器级别的信息
    int m_videoStreamIndex;          ///< 视频流索引，-1 表示未找到
    int m_audioStreamIndex;          ///< 音频流索引，-1 表示未找到
    int m_subtitleStreamIndex;       ///< 字幕流索引，-1 表示未找到
};

} // namespace FluxPlayer
