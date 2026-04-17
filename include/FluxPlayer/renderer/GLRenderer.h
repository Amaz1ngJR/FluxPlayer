/**
 * @file GLRenderer.h
 * @brief OpenGL 视频渲染器，负责将 YUV420P 视频帧渲染到屏幕
 */

#pragma once

#include "Shader.h"
#include <memory>
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

    /** @brief 销毁渲染器，释放所有 OpenGL 资源（VAO/VBO/纹理） */
    void destroy();

    /**
     * @brief 渲染一帧视频数据（支持 YUV420P 和 NV12 两种格式）
     * @param yData  Y（亮度）平面数据指针
     * @param uData  YUV420P: U平面 / NV12: UV交错平面
     * @param vData  YUV420P: V平面 / NV12: 不使用
     * @param yPitch Y 平面每行字节数（linesize）
     * @param uPitch YUV420P: U平面行字节数 / NV12: UV平面行字节数
     * @param vPitch YUV420P: V平面行字节数 / NV12: 不使用
     * @param isNV12 true = NV12 格式（硬件解码输出），false = YUV420P
     */
    void renderFrame(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                     int yPitch, int uPitch, int vPitch,
                     bool isNV12 = false);

    /**
     * @brief 设置 NV12 渲染是否使用 UV 解交错模式
     *
     * GL_RG8 纹理在某些 GPU/驱动上存在兼容性问题（如 CUDA + NVIDIA），
     * 此时需要将 NV12 的交错 UV 解交错为独立 U/V 平面后上传。
     * D3D11VA/DXVA2/VideoToolbox 等后端可使用 GL_RG8 真零拷贝。
     *
     * @param enable true = UV 解交错模式（兼容性优先），false = GL_RG8 模式（性能优先）
     */
    void setNV12Deinterleave(bool enable) { m_nv12Deinterleave = enable; }

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
     * 根据 m_nv12Deinterleave 标志选择策略：
     * - true:  UV 解交错后上传到独立 U/V 纹理（兼容模式，用于 CUDA）
     * - false: 直接上传到 GL_RG8 纹理（零拷贝模式，用于 D3D11VA 等）
     *
     * @param yData   Y 平面数据指针
     * @param uvData  UV 交错平面数据指针
     * @param yPitch  Y 平面行跨度
     * @param uvPitch UV 平面行跨度
     */
    void updateNV12Textures(uint8_t* yData, uint8_t* uvData, int yPitch, int uvPitch);

    std::unique_ptr<Shader> m_shader;   ///< YUV→RGB 转换着色器程序

    unsigned int m_VAO;       ///< 全屏四边形的顶点数组对象
    unsigned int m_VBO;       ///< 全屏四边形的顶点缓冲对象

    unsigned int m_textureY;  ///< Y（亮度）平面纹理，全分辨率
    unsigned int m_textureU;  ///< U（色度Cb）平面纹理，1/2 宽 x 1/2 高
    unsigned int m_textureV;  ///< V（色度Cr）平面纹理，1/2 宽 x 1/2 高
    unsigned int m_textureUV; ///< NV12 色度纹理（GL_RG8），保留用于未来 GL_RG8 兼容的 GPU

    int m_videoWidth;   ///< 当前视频帧宽度
    int m_videoHeight;  ///< 当前视频帧高度

    /// NV12 渲染模式：true = UV 解交错（CUDA 兼容），false = GL_RG8 零拷贝
    bool m_nv12Deinterleave = false;

    /// NV12 UV 解交错缓冲区（预分配，避免每帧动态分配）
    std::vector<uint8_t> m_nv12UBuffer;  ///< 解交错后的 U 平面
    std::vector<uint8_t> m_nv12VBuffer;  ///< 解交错后的 V 平面
};

} // namespace FluxPlayer
