/**
 * @file SubtitleDecoder.cpp
 * @brief 内嵌字幕流解码器实现
 *
 * 参考 VideoDecoder 的 init/close 模板，聚焦三件事：
 *   1. 正确创建并持有 AVCodecContext
 *   2. 解码后的 AVSubtitle 必须 100% 被释放（RAII scope guard）
 *   3. ASS 控制标签的清理要鲁棒（容错空字符串、极长内容、无配对括号）
 */

#include "FluxPlayer/subtitle/SubtitleDecoder.h"
#include "FluxPlayer/utils/Logger.h"

#include <cstring>

namespace FluxPlayer {

namespace {
/**
 * @brief AVSubtitle 作用域守卫，保证无论从哪个路径退出都会释放
 *
 * FFmpeg 的 avcodec_decode_subtitle2 在 gotSub == 1 时会填充 AVSubtitle，
 * 其 rects 是堆分配的，必须调用 avsubtitle_free 释放。
 * 使用 RAII 避免 early-return 路径漏释放。
 */
class AVSubtitleGuard {
public:
    explicit AVSubtitleGuard(AVSubtitle& s) : m_sub(s), m_active(true) {}
    ~AVSubtitleGuard() { if (m_active) avsubtitle_free(&m_sub); }
    AVSubtitleGuard(const AVSubtitleGuard&) = delete;
    AVSubtitleGuard& operator=(const AVSubtitleGuard&) = delete;
    /** 主动放弃释放责任（本项目未使用，保留接口对称性） */
    void release() { m_active = false; }
private:
    AVSubtitle& m_sub;
    bool m_active;
};
} // namespace

SubtitleDecoder::SubtitleDecoder()
    : m_codecCtx(nullptr)
    , m_timeBase{1, 1000} {
    LOG_DEBUG("SubtitleDecoder constructor called");
}

SubtitleDecoder::~SubtitleDecoder() {
    LOG_DEBUG("SubtitleDecoder destructor called");
    close();
}

bool SubtitleDecoder::init(AVCodecParameters* codecParams, AVRational timeBase) {
    // 防御：上游传入 null 直接失败，避免后续 FFmpeg API 崩溃
    if (!codecParams) {
        LOG_ERROR("SubtitleDecoder::init received nullptr codecParams");
        return false;
    }

    // 如果上次残留未清理（理论上不会发生），先 close 再重建
    if (m_codecCtx) {
        LOG_WARN("SubtitleDecoder::init called with an active context, closing first");
        close();
    }

    // 1. 查找解码器（SRT/ASS/WebVTT/mov_text 等 FFmpeg 内建支持）
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LOG_ERROR("Subtitle decoder not found for codec id: " +
                  std::to_string(static_cast<int>(codecParams->codec_id)));
        return false;
    }
    LOG_INFO("Subtitle codec: " + std::string(codec->name) +
             " (" + std::string(codec->long_name ? codec->long_name : "") + ")");

    // 2. 分配解码器上下文
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        LOG_ERROR("Failed to allocate subtitle codec context");
        return false;
    }

    // 3. 将流参数复制到上下文
    int ret = avcodec_parameters_to_context(m_codecCtx, codecParams);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to copy subtitle codec params: " + std::string(errBuf));
        close();
        return false;
    }

    // 4. 打开解码器
    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_ERROR("Failed to open subtitle codec: " + std::string(errBuf));
        close();
        return false;
    }

    // 5. 保存时间基准（packet.pts 的单位）
    m_timeBase = timeBase;
    LOG_INFO("SubtitleDecoder initialized, time_base=" +
             std::to_string(timeBase.num) + "/" + std::to_string(timeBase.den));
    return true;
}

std::vector<SubtitleDecoder::Item> SubtitleDecoder::decode(AVPacket* packet) {
    std::vector<Item> result;

    // 参数 / 状态防御
    if (!m_codecCtx) {
        LOG_DEBUG("SubtitleDecoder::decode called on uninitialized decoder");
        return result;
    }
    if (!packet || packet->size <= 0) {
        // 空包在 EOF 刷新或异常流中可能出现，不是错误
        return result;
    }

    // FFmpeg 要求 AVSubtitle 先清零
    AVSubtitle sub{};
    int gotSub = 0;
    int ret = avcodec_decode_subtitle2(m_codecCtx, &sub, &gotSub, packet);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_WARN("avcodec_decode_subtitle2 failed: " + std::string(errBuf));
        // 部分解码器在失败时不分配 rects，但稳妥起见仍 free
        avsubtitle_free(&sub);
        return result;
    }

    // RAII 守卫：从这里起，任何 return 路径都会释放 AVSubtitle
    AVSubtitleGuard guard(sub);

    if (!gotSub || sub.num_rects == 0) {
        return result;
    }

    // === 计算 PTS ===
    // packet.pts 可能为 AV_NOPTS_VALUE（INT64_MIN），直接换算会得到 -9.2e18，
    // 导致所有后续比较都失效，故必须先判断
    double startPTS = 0.0;
    if (packet->pts != AV_NOPTS_VALUE) {
        startPTS = static_cast<double>(packet->pts) * av_q2d(m_timeBase);
    } else if (packet->dts != AV_NOPTS_VALUE) {
        // 退而求其次使用 DTS（部分 demuxer 只写 DTS）
        startPTS = static_cast<double>(packet->dts) * av_q2d(m_timeBase);
    }

    // end_display_time 是相对 startPTS 的毫秒数
    // 优先级：end_display_time > packet->duration > 兜底 5 秒
    // mov_text/SRT 封装时 end_display_time 常为 0，需用 packet->duration 补偿
    double displayDurationSec;
    if (sub.end_display_time > 0) {
        displayDurationSec = sub.end_display_time / 1000.0;
    } else if (packet->duration > 0) {
        displayDurationSec = static_cast<double>(packet->duration) * av_q2d(m_timeBase);
    } else {
        displayDurationSec = 5.0;
    }
    double endPTS = startPTS + displayDurationSec;

    LOG_DEBUG("Subtitle PTS: start=" + std::to_string(startPTS) +
              " end=" + std::to_string(endPTS) +
              " timeBase=" + std::to_string(m_timeBase.num) + "/" + std::to_string(m_timeBase.den) +
              " raw_pts=" + std::to_string(packet->pts) +
              " end_display_time=" + std::to_string(sub.end_display_time));

    // === 遍历所有 rect，提取文本 ===
    result.reserve(sub.num_rects);
    for (unsigned i = 0; i < sub.num_rects; ++i) {
        AVSubtitleRect* rect = sub.rects[i];
        if (!rect) continue;

        const char* raw = nullptr;
        switch (rect->type) {
            case SUBTITLE_ASS:
                raw = rect->ass;
                break;
            case SUBTITLE_TEXT:
                raw = rect->text;
                break;
            case SUBTITLE_BITMAP:
                // PGS/DVB 等图形字幕，阶段一跳过
                LOG_DEBUG("Skipping bitmap subtitle rect (index " +
                          std::to_string(i) + ")");
                continue;
            default:
                LOG_DEBUG("Skipping unknown subtitle rect type: " +
                          std::to_string(static_cast<int>(rect->type)));
                continue;
        }

        if (!raw || raw[0] == '\0') continue;

        std::string cleaned = stripASSOverrides(raw);
        if (cleaned.empty()) continue;

        result.push_back(Item{std::move(cleaned), startPTS, endPTS});
    }

    return result;
}

void SubtitleDecoder::flush() {
    if (m_codecCtx) {
        avcodec_flush_buffers(m_codecCtx);
        LOG_DEBUG("SubtitleDecoder flushed");
    }
}

void SubtitleDecoder::close() {
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
        LOG_DEBUG("SubtitleDecoder closed");
    }
}

std::string SubtitleDecoder::stripASSOverrides(const std::string& raw) {
    static constexpr size_t kMaxTextSize = 4 * 1024;  // 防御：单条字幕超过 4KB 视为异常

    if (raw.empty()) return {};

    // --- Step 1: 若是 "Dialogue: ..." 格式，跳过前 9 个逗号字段（标准 ASS Event 列数）---
    // FFmpeg 4.x 的 rects[i]->ass 往往是完整 Dialogue 行；FFmpeg 5+ 已直接给文本部分
    std::string body;
    if (raw.compare(0, 9, "Dialogue:") == 0) {
        size_t pos = 0;
        int commasToSkip = 9;
        while (commasToSkip > 0 && pos < raw.size()) {
            size_t c = raw.find(',', pos);
            if (c == std::string::npos) break;
            pos = c + 1;
            --commasToSkip;
        }
        body = (pos < raw.size()) ? raw.substr(pos) : std::string();
    } else {
        body = raw;
    }

    // --- Step 2: 去除 `{...}` override + 处理转义换行 ---
    std::string out;
    out.reserve(body.size());

    bool inTag = false;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];

        if (inTag) {
            // 遇到 } 退出标签；遇到行尾也强制退出（容错）
            if (c == '}') inTag = false;
            else if (c == '\n') { inTag = false; out += '\n'; }
            continue;
        }

        if (c == '{') {
            inTag = true;
            continue;
        }

        // ASS 转义：\N（硬换行）、\n（软换行，渲染器通常也作硬换行处理）、\h（硬空格）
        if (c == '\\' && i + 1 < body.size()) {
            char next = body[i + 1];
            if (next == 'N' || next == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (next == 'h') {
                out += ' ';
                ++i;
                continue;
            }
        }

        out += c;
    }

    // --- Step 3: trim 尾部空白 + 长度上限防御 ---
    while (!out.empty() &&
           (out.back() == ' ' || out.back() == '\n' ||
            out.back() == '\r' || out.back() == '\t')) {
        out.pop_back();
    }

    if (out.size() > kMaxTextSize) {
        out.resize(kMaxTextSize);
    }

    return out;
}

} // namespace FluxPlayer
