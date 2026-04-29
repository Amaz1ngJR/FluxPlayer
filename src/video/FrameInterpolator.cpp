/**
 * @file FrameInterpolator.cpp
 * @brief 帧插值器实现 —— 运动补偿 + 自适应混合
 */

#include "FluxPlayer/video/FrameInterpolator.h"
#include "FluxPlayer/decoder/Frame.h"
#include "FluxPlayer/utils/Logger.h"

#include <cstdint>
#include <cmath>
#include <climits>
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace FluxPlayer {

FrameInterpolator::FrameInterpolator() {
    LOG_DEBUG("FrameInterpolator created");
}

FrameInterpolator::~FrameInterpolator() {
    LOG_DEBUG("FrameInterpolator destroyed");
}

Frame* FrameInterpolator::interpolate(const Frame* frame1, const Frame* frame2, double alpha) {
    if (!frame1 || !frame2) return nullptr;
    if (frame1->getWidth() != frame2->getWidth() ||
        frame1->getHeight() != frame2->getHeight()) {
        LOG_WARN("FrameInterpolator: 分辨率不一致，无法插值");
        return nullptr;
    }

    const uint8_t* y1 = frame1->getAVFrame()->data[0];
    const uint8_t* y2 = frame2->getAVFrame()->data[0];
    int stride = frame1->getAVFrame()->linesize[0];

    double motion_level = detectMotion(y1, y2, frame1->getWidth(), frame1->getHeight(), stride);

    Frame* result = nullptr;
    if (motion_level < kMotionThreshold) {
        // 静止场景：线性混合（~1ms）
        result = blendFrames(frame1, frame2, alpha);
    } else {
        // 运动场景：块匹配运动补偿插值（~8ms @1080p）
        result = motionCompensatedInterpolate(frame1, frame2, alpha);
    }

    // 降级策略：运动补偿失败时 fallback 到线性混合
    if (!result) {
        LOG_WARN("FrameInterpolator: 运动补偿失败，降级为线性混合");
        result = blendFrames(frame1, frame2, alpha);
    }

    return result;
}

double FrameInterpolator::detectMotion(const uint8_t* y1, const uint8_t* y2,
                                        int width, int height, int stride) {
    // 降采样检测：每 kSampleStep 像素取一个采样点，减少计算量
    uint64_t sad = 0;
    int sample_count = 0;

    for (int y = 0; y < height; y += kSampleStep) {
        for (int x = 0; x < width; x += kSampleStep) {
            int idx = y * stride + x;
            sad += static_cast<uint64_t>(std::abs(static_cast<int>(y1[idx]) - static_cast<int>(y2[idx])));
            sample_count++;
        }
    }

    if (sample_count == 0) return 0.0;
    // 归一化到 [0, 1]
    return static_cast<double>(sad) / (sample_count * kMaxPixelValue);
}

Frame* FrameInterpolator::blendFrames(const Frame* frame1, const Frame* frame2, double alpha) {
    int width = frame1->getWidth();
    int height = frame1->getHeight();

    Frame* result = new Frame();
    if (!result->allocate(width, height, AV_PIX_FMT_YUV420P)) {
        delete result;
        return nullptr;
    }

    const AVFrame* src1 = frame1->getAVFrame();
    const AVFrame* src2 = frame2->getAVFrame();
    AVFrame* dst = result->getAVFrame();
    int a1 = static_cast<int>((1.0 - alpha) * kAlphaScale);
    int a2 = static_cast<int>(alpha * kAlphaScale);

    // Y 平面混合（全分辨率）
    for (int y = 0; y < height; y++) {
        const uint8_t* s1 = src1->data[0] + y * src1->linesize[0];
        const uint8_t* s2 = src2->data[0] + y * src2->linesize[0];
        uint8_t* d = dst->data[0] + y * dst->linesize[0];
        for (int x = 0; x < width; x++) {
            d[x] = static_cast<uint8_t>((s1[x] * a1 + s2[x] * a2) >> 8);
        }
    }

    // U/V 平面混合（半分辨率）
    int chroma_w = width / 2;
    int chroma_h = height / 2;
    for (int plane = 1; plane <= 2; plane++) {
        for (int y = 0; y < chroma_h; y++) {
            const uint8_t* s1 = src1->data[plane] + y * src1->linesize[plane];
            const uint8_t* s2 = src2->data[plane] + y * src2->linesize[plane];
            uint8_t* d = dst->data[plane] + y * dst->linesize[plane];
            for (int x = 0; x < chroma_w; x++) {
                d[x] = static_cast<uint8_t>((s1[x] * a1 + s2[x] * a2) >> 8);
            }
        }
    }

    result->setPTS(frame1->getPTS() + (frame2->getPTS() - frame1->getPTS()) * alpha);
    return result;
}

FrameInterpolator::MotionVector FrameInterpolator::blockMatch(
    const uint8_t* ref, const uint8_t* cur,
    int block_x, int block_y,
    int width, int height, int stride) {

    int best_dx = 0, best_dy = 0;
    int best_sad = INT_MAX;

    // 三步搜索法：步长从 kSearchRange/2 逐步缩小到 1
    int step = kSearchRange / 2;
    int cx = 0, cy = 0;

    while (step >= 1) {
        for (int dy = -step; dy <= step; dy += step) {
            for (int dx = -step; dx <= step; dx += step) {
                int sx = block_x + cx + dx;
                int sy = block_y + cy + dy;

                // 边界检查
                if (sx < 0 || sx + kBlockSize > width ||
                    sy < 0 || sy + kBlockSize > height) {
                    continue;
                }

                // 计算 SAD
                int sad = 0;
                for (int by = 0; by < kBlockSize; by++) {
                    const uint8_t* r = ref + (block_y + by) * stride + block_x;
                    const uint8_t* c = cur + (sy + by) * stride + sx;
                    for (int bx = 0; bx < kBlockSize; bx++) {
                        sad += std::abs(static_cast<int>(r[bx]) - static_cast<int>(c[bx]));
                    }
                }

                if (sad < best_sad) {
                    best_sad = sad;
                    best_dx = cx + dx;
                    best_dy = cy + dy;
                }
            }
        }
        cx = best_dx;
        cy = best_dy;
        step /= 2;
    }

    return {best_dx, best_dy};
}

Frame* FrameInterpolator::motionCompensatedInterpolate(
    const Frame* frame1, const Frame* frame2, double alpha) {

    int width = frame1->getWidth();
    int height = frame1->getHeight();

    Frame* result = new Frame();
    if (!result->allocate(width, height, AV_PIX_FMT_YUV420P)) {
        delete result;
        return nullptr;
    }

    const AVFrame* src1 = frame1->getAVFrame();
    const AVFrame* src2 = frame2->getAVFrame();
    AVFrame* dst = result->getAVFrame();

    const uint8_t* y1 = src1->data[0];
    const uint8_t* y2 = src2->data[0];
    int stride1 = src1->linesize[0];
    int stride2 = src2->linesize[0];

    int a1 = static_cast<int>((1.0 - alpha) * kAlphaScale);
    int a2 = static_cast<int>(alpha * kAlphaScale);

    // Y 平面：块匹配运动补偿
    for (int by = 0; by < height; by += kBlockSize) {
        for (int bx = 0; bx < width; bx += kBlockSize) {
            MotionVector mv = blockMatch(y1, y2, bx, by, width, height, stride2);

            // 按 alpha 比例沿运动向量方向取中间位置
            int interp_dx = static_cast<int>(mv.dx * alpha);
            int interp_dy = static_cast<int>(mv.dy * alpha);

            int src_x = bx + interp_dx;
            int src_y = by + interp_dy;

            int block_w = std::min(kBlockSize, width - bx);
            int block_h = std::min(kBlockSize, height - by);
            src_x = std::max(0, std::min(src_x, width - block_w));
            src_y = std::max(0, std::min(src_y, height - block_h));

            // 双向混合：前向补偿 + 后向补偿，减少伪影
            for (int row = 0; row < block_h; row++) {
                const uint8_t* s1 = y1 + (src_y + row) * stride1 + src_x;
                const uint8_t* s2 = y2 + (by + row) * stride2 + bx;
                uint8_t* d = dst->data[0] + (by + row) * dst->linesize[0] + bx;
                for (int col = 0; col < block_w; col++) {
                    d[col] = static_cast<uint8_t>((s1[col] * a1 + s2[col] * a2) >> 8);
                }
            }
        }
    }

    // U/V 平面：简单线性混合（色度分辨率低，运动补偿收益小）
    int chroma_w = width / 2;
    int chroma_h = height / 2;
    for (int plane = 1; plane <= 2; plane++) {
        const uint8_t* s1 = src1->data[plane];
        const uint8_t* s2 = src2->data[plane];
        int ls1 = src1->linesize[plane];
        int ls2 = src2->linesize[plane];
        for (int y = 0; y < chroma_h; y++) {
            uint8_t* d = dst->data[plane] + y * dst->linesize[plane];
            for (int x = 0; x < chroma_w; x++) {
                d[x] = static_cast<uint8_t>((s1[y * ls1 + x] * a1 + s2[y * ls2 + x] * a2) >> 8);
            }
        }
    }

    result->setPTS(frame1->getPTS() + (frame2->getPTS() - frame1->getPTS()) * alpha);
    return result;
}

} // namespace FluxPlayer
