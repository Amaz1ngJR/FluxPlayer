/**
 * @file FrameInterpolator.h
 * @brief 帧插值器 —— 慢放时在两帧之间生成中间帧
 */

#pragma once

namespace FluxPlayer {

class Frame;  // 前向声明，避免暴露 FFmpeg 类型

/**
 * @brief 帧插值器 —— 慢放时在两帧之间生成中间帧
 *
 * 自适应策略：
 * - 静止场景（SAD < kMotionThreshold）：线性混合，~1ms
 * - 运动场景：块匹配运动补偿插值，~8ms @1080p
 * - 插值失败时自动降级为简单重复帧
 */
class FrameInterpolator {
public:
    FrameInterpolator();
    ~FrameInterpolator();

    /**
     * @brief 在两帧之间插值生成中间帧
     * @param frame1 前一帧
     * @param frame2 后一帧
     * @param alpha 插值位置（0.0 = frame1, 1.0 = frame2, 0.5 = 中点）
     * @return 插值帧（调用者负责释放），失败返回 nullptr
     */
    Frame* interpolate(const Frame* frame1, const Frame* frame2, double alpha);

private:
    static constexpr double kMotionThreshold = 0.02;  // 运动检测阈值（SAD 归一化值）
    static constexpr int kSampleStep = 4;              // 运动检测降采样步长
    static constexpr int kBlockSize = 16;              // 块匹配块大小（像素）
    static constexpr int kSearchRange = 16;            // 块匹配搜索范围（像素）
    static constexpr int kAlphaScale = 256;            // alpha 混合定点化缩放因子
    static constexpr int kMaxPixelValue = 255;         // 像素最大值（8-bit）

    struct MotionVector { int dx, dy; };

    /**
     * @brief 检测两帧之间的运动强度
     * @return 归一化 SAD 值（0.0 = 完全静止，1.0 = 完全不同）
     */
    double detectMotion(const uint8_t* y1, const uint8_t* y2,
                        int width, int height, int stride);

    /**
     * @brief 线性混合两帧（用于静止场景）
     * @param frame1 前一帧
     * @param frame2 后一帧
     * @param alpha 混合权重
     * @return 混合后的帧，失败返�� nullptr
     */
    Frame* blendFrames(const Frame* frame1, const Frame* frame2, double alpha);

    /**
     * @brief 块匹配运动补偿插值（用于运动场景）
     * @param frame1 前一帧
     * @param frame2 后一帧
     * @param alpha 插值位置
     * @return 插值帧，失败返回 nullptr
     */
    Frame* motionCompensatedInterpolate(const Frame* frame1, const Frame* frame2, double alpha);

    /**
     * @brief 块匹配搜索 —— 三步搜索法
     * @param ref 参考帧 Y 平面
     * @param cur 当前帧 Y 平面
     * @param block_x 块起始 X 坐标
     * @param block_y 块起始 Y 坐标
     * @param width 帧宽度
     * @param height 帧高度
     * @param stride 行跨度
     * @return 运动向量（相对于参考块的偏移）
     */
    MotionVector blockMatch(const uint8_t* ref, const uint8_t* cur,
                            int block_x, int block_y,
                            int width, int height, int stride);
};

} // namespace FluxPlayer
