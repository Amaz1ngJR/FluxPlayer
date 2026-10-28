#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FluxPlayer {
/// 私有模型适配器：固定 256 tile、32 像素 halo，输出有效 RGB24 图像。
class RealESRGAN {
public:
    RealESRGAN();
    ~RealESRGAN();
    RealESRGAN(const RealESRGAN&) = delete;
    RealESRGAN& operator=(const RealESRGAN&) = delete;
    /// 校验模型和运行库；Mac 自动模式先尝试快速 CoreML，再标准 CoreML/CPU；失败抛异常。
    void load(const std::string& library, const std::string& model, int scale, bool cpu,
              bool externalModel = false);
    /// 逐块增强；previousRgb 对应 result 中上一成功帧，仅完整输入块相同才复用。
    void enhance(const std::vector<uint8_t>& rgb, int width, int height,
                 std::vector<uint8_t>& result, const std::atomic<bool>& cancelled,
                 const std::vector<uint8_t>& previousRgb);
    uint64_t reusedTiles() const;
    /// 返回模型加载后的原生倍率（外部模型通过实际推理输出识别）。
    int scale() const;
    /// 实际会话 EP 描述，不宣称全部 GPU/ANE。
    std::string device() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
