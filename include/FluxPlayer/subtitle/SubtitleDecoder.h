/**
 * @file SubtitleDecoder.h
 * @brief 内嵌字幕流解码器
 *
 * 基于 FFmpeg libavcodec 的 `avcodec_decode_subtitle2` API，负责将
 * Demuxer 读取出的字幕 AVPacket 解码为可供 UI 渲染的纯文本条目。
 * 阶段一仅支持文本类字幕（SRT / ASS / SSA / WebVTT / mov_text），
 * 图形字幕（PGS / DVB-SUB）会被忽略并记录 DEBUG 日志。
 *
 * 头文件依赖说明：
 *   本头与 VideoDecoder.h 同级（解码层内部头），允许 include FFmpeg；
 *   不会被 UI 层直接包含（UI 层仅通过 SubtitleManager 拿字幕文本）。
 */

#pragma once

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace FluxPlayer {

/**
 * @brief 字幕解码器，将压缩字幕包解码为带时间戳的文本条目
 *
 * 与 VideoDecoder / AudioDecoder 风格一致：RAII 管理 AVCodecContext，
 * 禁止拷贝，`init/decode/flush/close` 四段式接口。
 */
class SubtitleDecoder {
public:
    /**
     * @brief 单个字幕条目的解码结果
     *
     * 一个 AVPacket 可能产出多条 rect（不同位置或不同样式），
     * decode() 将每条 rect 扁平化为一个 Item 返回。
     */
    struct Item {
        std::string text;   ///< 已去除 ASS 控制标签的纯文本（可能含 `\n` 换行）
        double startPTS;    ///< 起始显示时间（秒，相对流起点）
        double endPTS;      ///< 结束显示时间（秒，相对流起点）
    };

    SubtitleDecoder();
    ~SubtitleDecoder();

    // 禁止拷贝，防止多次 avcodec_free_context 导致 double-free
    SubtitleDecoder(const SubtitleDecoder&) = delete;
    SubtitleDecoder& operator=(const SubtitleDecoder&) = delete;

    /**
     * @brief 初始化解码器
     *
     * 调用 FFmpeg 查找 / 分配 / 打开字幕解码器。
     * @param codecParams 来自 Demuxer 的字幕 AVCodecParameters
     * @param timeBase    字幕流 time_base，用于 packet.pts → 秒 的换算
     * @return 初始化成功返回 true；失败时内部已回滚，可直接销毁对象
     */
    bool init(AVCodecParameters* codecParams, AVRational timeBase);

    /**
     * @brief 解码一个字幕数据包
     *
     * 处理流程：
     *   1. 参数与空包防御
     *   2. 调用 avcodec_decode_subtitle2，RAII 确保 avsubtitle_free
     *   3. 遍历 rects：取 SUBTITLE_ASS / SUBTITLE_TEXT 的文本字段
     *   4. 通过 stripASSOverrides 清理控制标签
     *   5. 计算 startPTS / endPTS（end_display_time 为 0 时给 5 秒默认显示）
     *
     * @param packet 待解码的字幕 AVPacket（不会被拷贝，也不会被 free）
     * @return 解码得到的文本条目列表；无有效结果时返回空 vector
     */
    std::vector<Item> decode(AVPacket* packet);

    /**
     * @brief 刷新解码器缓冲区
     *
     * 在 seek 后调用，避免解码出 seek 前的残留字幕。
     */
    void flush();

    /**
     * @brief 关闭解码器并释放上下文（可重复调用）
     */
    void close();

private:
    /**
     * @brief 清理 ASS/SSA 控制标签，返回适合显示的纯文本
     *
     * 处理要点：
     *   - 若字符串以 "Dialogue:" 开头（FFmpeg 4.x 行为），跳过前 9 个逗号字段
     *   - `{\\xxx}` 包裹的 override 标签被整段跳过（容错：无 `}` 时跳到行尾）
     *   - `\\N` / `\\n` 转 `\n`；`\\h` 转空格
     *   - 尾部空白 trim；过长文本截断为 4 KB（防御恶意字幕）
     */
    static std::string stripASSOverrides(const std::string& raw);

    AVCodecContext* m_codecCtx;     ///< 字幕解码器上下文，null = 未初始化
    AVRational m_timeBase;          ///< 字幕流时间基准，用于 PTS 换算
};

} // namespace FluxPlayer
