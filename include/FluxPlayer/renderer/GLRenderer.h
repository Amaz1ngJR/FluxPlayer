/**
 * @file GLRenderer.h
 * @brief OpenGL 视频渲染器，负责将 YUV420P 视频帧渲染到屏幕
 */

#pragma once

#include "HardwareFrameInterop.h"
#include "Shader.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FluxPlayer {

/**
 * @brief OpenGL 视频渲染器
 *
 * 使用 OpenGL 纹理和着色器将 YUV420P 格式的视频帧渲染到全屏四边形上。
 * 内部维护 Y/U/V 三个独立纹理，在片段着色器中完成 YUV→RGB 色彩空间转换。
 */
class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    /**
     * @brief 初始化渲染器：加载着色器、创建全屏四边形、分配 YUV 纹理
     * @param videoWidth  视频帧宽度（像素）
     * @param videoHeight 视频帧高度（像素）
     * @return 成功返回 true，失败返回 false
     */
    bool init(int videoWidth, int videoHeight);

    /**
     * @brief 初始化当前硬件解码设备与 OpenGL 之间的零 CPU 拷贝互操作
     *
     * 必须在 OpenGL 上下文创建后、解码线程启动前调用。失败时调用方应整体切换到
     * 软件解码，不能继续硬解后通过 CPU 下载帧。
     *
     * @param deviceType FFmpeg 硬件设备类型
     * @param hwDeviceContext FFmpeg 硬件设备上下文
     * @return 原生互操作链路可用时返回 true
     */
    bool initHardwareInterop(AVHWDeviceType deviceType, AVBufferRef* hwDeviceContext);

    /**
     * @brief 硬件帧与 OpenGL 的原生互操作链路是否已成功初始化
     *
     * 该值表示链路具备工作条件，不等同于当前画面已经使用硬件帧。实际状态需要
     * 结合 isHardwareFrameActive()，以便软件帧回退后状态面板不会误报零拷贝。
     */
    bool isHardwareInteropReady() const { return m_hardwareInterop != nullptr; }

    /**
     * @brief 当前缓存画面是否来自硬件帧原生互操作
     *
     * 只有 beginFrame() 成功映射硬件 AVFrame 后才为 true；只要之后渲染了
     * CPU 内存帧或静态图片，就会立即恢复为 false。
     */
    bool isHardwareFrameActive() const {
        return m_hardwareInterop != nullptr && m_lastFrameWasHardware;
    }

    /** @brief 返回当前已初始化的硬件解码后端名称，未初始化时返回 Software */
    std::string hardwareBackendName() const {
        return m_hardwareInterop ? m_hardwareInterop->backendName() : "Software";
    }

    /** @brief 返回当前硬件设备名称，软件路径返回 CPU */
    std::string hardwareDeviceName() const {
        return m_hardwareInterop ? m_hardwareInterop->deviceName() : "CPU";
    }

    /** @brief 返回硬件帧到 OpenGL 的零拷贝互操作方式 */
    std::string zeroCopyModeName() const {
        return m_hardwareInterop ? m_hardwareInterop->zeroCopyMode() : "Disabled";
    }

    /**
     * @brief 设置显示亮度。
     * @param brightness 亮度倍率，0.25~2.0，1.0 表示保持原始画面
     *
     * 仅更新 CPU 侧 uniform 缓存，实际应用发生在拥有 OpenGL 上下文的渲染调用中；
     * 不修改解码帧，因此软件上传和硬件零拷贝路径均不会增加 CPU 像素拷贝。
     */
    void setBrightness(float brightness);
    float brightness() const { return m_brightness; }

    /** @brief 销毁渲染器，释放所有 OpenGL 资源（VAO/VBO/纹理） */
    void destroy();

    /**
     * @brief 渲染一个完整 AVFrame
     *
     * D3D11/VideoToolbox 帧会直接走平台 GPU 互操作；软件 YUV/NV12 帧才允许使用
     * CPU plane 上传。硬件帧互操作失败时返回 false，绝不偷偷执行 GPU->CPU 下载。
     */
    bool renderFrame(const AVFrame* frame, int colorSpace = 0, int fullRange = 0);

    /**
     * @brief 渲染一帧视频数据（支持 YUV420P 和 NV12 两种格式）
     * @param yData  Y（亮度）平面数据指针
     * @param uData  YUV420P: U平面 / NV12: UV交错平面
     * @param vData  YUV420P: V平面 / NV12: 不使用
     * @param yPitch Y 平面每行字节数（linesize）
     * @param uPitch YUV420P: U平面行字节数 / NV12: UV平面行字节数
     * @param vPitch YUV420P: V平面行字节数 / NV12: 不使用
     * @param isNV12 true = CPU 内存 NV12，false = CPU 内存 YUV420P
     * @param colorSpace 色彩空间：0=BT.601, 1=BT.709, 2=BT.2020
     * @param fullRange  量化范围：0=TV/limited, 1=PC/full
     */
    void renderFrame(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                     int yPitch, int uPitch, int vPitch,
                     bool isNV12 = false,
                     int colorSpace = 0, int fullRange = 0);

    /**
     * @brief 设置 NV12 渲染是否使用 UV 解交错模式
     *
     * GL_RG8 纹理在少数旧驱动上存在兼容性问题，此时需要将软件 NV12 的
     * 交错 UV 解交错为独立 U/V 平面后上传。
     * 该设置只影响已经位于 CPU 内存中的 NV12 软件帧。D3D11VA/VideoToolbox
     * 硬件帧由 HardwareFrameInterop 直接导入，不经过本接口。
     *
     * @param enable true = UV 解交错模式（兼容性优先），false = GL_RG8 模式（性能优先）
     */
    void setNV12Deinterleave(bool enable) { m_nv12Deinterleave = enable; }

    /**
     * @brief 查询 GPU 纹理中是否有可用的视频帧数据
     *
     * 渲染后帧数据已上传到 GPU 纹理，CPU 侧可释放像素引用。
     * 暂停时可直接复用纹理中的数据，无需 CPU 侧帧。
     *
     * @return true = 纹理中有有效帧数据可供渲染
     */
    bool hasValidTexture() const { return m_hasValidTexture; }

    /**
     * @brief 使用已有的 GPU 纹理数据重新渲染（不上传新数据）
     *
     * 用于暂停、队列为空等场景，避免持有 CPU 侧帧数据。
     */
    void renderCachedFrame();

    /**
     * @brief 使用已有的 GPU 纹理数据重新渲染，支持缩放和偏移
     *
     * 用于截图效果：画面缩小并移动到角落
     * @param scale 缩放比例（1.0 = 原始大小）
     * @param offsetX X 轴偏移比例（0.0 = 原位，1.0 = 屏幕右侧）
     * @param offsetY Y 轴偏移比例（0.0 = 原位，1.0 = 屏幕底部）
     * @param alpha 透明度（1.0 = 完全不透明，0.0 = 完全透明）
     */
    void renderCachedFrameWithTransform(float scale, float offsetX, float offsetY, float alpha);

    /**
     * @brief 清除屏幕为指定颜色
     * @param r 红色分量 (0.0~1.0)
     * @param g 绿色分量 (0.0~1.0)
     * @param b 蓝色分量 (0.0~1.0)
     * @param a 透明度 (0.0~1.0)
     */
    void clear(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f);

    /**
     * @brief 更新视频尺寸，重新分配纹理内存
     * @param width  新的视频宽度（像素）
     * @param height 新的视频高度（像素）
     */
    void setVideoSize(int width, int height);

    /**
     * @brief 渲染静态 RGBA 图片（纯音频模式封面图）
     * @param rgbaData RGBA 像素数据（宽×高×4 字节）
     * @param width    图片宽度（像素）
     * @param height   图片高度（像素）
     *
     * 首次调用时创建纹理并上传数据；后续调用直接复用已上传的纹理。
     */
    void renderStaticImage(const uint8_t* rgbaData, int width, int height);

private:
    /** @brief 创建全屏四边形的 VAO/VBO，包含顶点位置和纹理坐标 */
    void setupQuad();

    /**
     * @brief 将 YUV 数据上传到 GPU 纹理，处理 pitch 与宽度不一致的对齐问题
     * @param yData, uData, vData YUV 三平面数据指针
     * @param yPitch, uPitch, vPitch 各平面的行跨度
     */
    void updateYUVTextures(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                          int yPitch, int uPitch, int vPitch);

    /**
     * @brief 将 NV12 数据上传到 GPU 纹理
     *
     * 根据 m_nv12Deinterleave 标志选择策略。该函数只处理 CPU 内存帧：
     * - true:  UV 解交错后上传到独立 U/V 纹理（旧驱动兼容模式）
     * - false: 直接上传到 GL_RG8 纹理（仍然是 CPU->GPU 上传）
     *
     * @param yData   Y 平面数据指针
     * @param uvData  UV 交错平面数据指针
     * @param yPitch  Y 平面行跨度
     * @param uvPitch UV 平面行跨度
     */
    void updateNV12Textures(uint8_t* yData, uint8_t* uvData, int yPitch, int uvPitch);

    /** @brief 绘制 beginFrame() 返回的原生 GPU 纹理，不接触 CPU 像素 */
    void drawHardwareBinding(const HardwareTextureBinding& binding,
                             int colorSpace,
                             int fullRange);

    std::unique_ptr<Shader> m_shader;   ///< YUV→RGB 转换着色器程序
    std::unique_ptr<Shader> m_hardwareYuvShader; ///< macOS 矩形 NV12 纹理着色器
    std::unique_ptr<HardwareFrameInterop> m_hardwareInterop; ///< 平台原生 GPU 互操作

    unsigned int m_VAO;       ///< 全屏四边形的顶点数组对象
    unsigned int m_VBO;       ///< 全屏四边形的顶点缓冲对象

    unsigned int m_textureY;  ///< Y（亮度）平面纹理，全分辨率
    unsigned int m_textureU;  ///< U（色度Cb）平面纹理，1/2 宽 x 1/2 高
    unsigned int m_textureV;  ///< V（色度Cr）平面纹理，1/2 宽 x 1/2 高
    unsigned int m_textureUV; ///< CPU 内存 NV12 的色度上传纹理（GL_RG8）

    int m_videoWidth;   ///< 当前视频帧宽度
    int m_videoHeight;  ///< 当前视频帧高度

    /// CPU NV12 上传模式：true = UV 解交错，false = 直接上传 GL_RG8
    bool m_nv12Deinterleave = false;

    /// GPU 纹理中是否有有效帧数据（首次 renderFrame 后置 true，setVideoSize 时重置）
    bool m_hasValidTexture = false;

    /// 上一次渲染使用的着色器模式（true = NV12 GL_RG8, false = YUV420P/NV12 解交错）
    bool m_lastIsNV12Shader = false;

    /// 上一次渲染的色彩空间（用于 renderCachedFrame 复用）
    int m_lastColorSpace = 0;
    /// 上一次渲染的量化范围（用于 renderCachedFrame 复用）
    int m_lastFullRange = 0;

    /// 上一次画面来自硬件互操作纹理，缓存重绘时必须重新取得跨 API 访问权
    bool m_lastFrameWasHardware = false;

    /// 显示亮度倍率；只作为 shader uniform 使用，不改变硬件帧零拷贝属性。
    float m_brightness = 1.0f;

    /// NV12 UV 解交错缓冲区（预分配，避免每帧动态分配）
    std::vector<uint8_t> m_nv12UBuffer;  ///< 解交错后的 U 平面
    std::vector<uint8_t> m_nv12VBuffer;  ///< 解交错后的 V 平面

    std::unique_ptr<Shader> m_rgbaShader;       ///< RGBA 直通着色器（纯音频封面图用）
    unsigned int m_textureRGBA{0};              ///< RGBA 封面图纹理
    bool m_staticImageUploaded{false};          ///< 封面图是否已上传到 GPU
};

} // namespace FluxPlayer
