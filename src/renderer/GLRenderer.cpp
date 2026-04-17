/**
 * GLRenderer.cpp - OpenGL 视频渲染器实现
 *
 * 功能：使用 OpenGL 渲染 YUV420P 格式的视频帧
 * 技术要点：
 * - YUV 三平面纹理管理
 * - 片段着色器中进行 YUV→RGB 色彩空间转换
 * - VAO/VBO 管理全屏四边形
 */

#include "FluxPlayer/renderer/GLRenderer.h"
#include "FluxPlayer/utils/Logger.h"

namespace FluxPlayer {

GLRenderer::GLRenderer()
    : m_VAO(0)
    , m_VBO(0)
    , m_textureY(0)
    , m_textureU(0)
    , m_textureV(0)
    , m_textureUV(0)
    , m_videoWidth(0)
    , m_videoHeight(0) {
    LOG_DEBUG("GLRenderer constructor called");
}

GLRenderer::~GLRenderer() {
    LOG_DEBUG("GLRenderer destructor called");
    destroy();
}

/**
 * 初始化 OpenGL 渲染器
 * @param videoWidth 视频宽度
 * @param videoHeight 视频高度
 * @return 成功返回 true，失败返回 false
 */
bool GLRenderer::init(int videoWidth, int videoHeight) {
    m_videoWidth = videoWidth;
    m_videoHeight = videoHeight;

    LOG_INFO("Initializing OpenGL renderer for " + std::to_string(videoWidth) + "x" +
             std::to_string(videoHeight) + " video");

    // 步骤1：加载并编译着色器程序
    // 顶点着色器：处理顶点位置
    // 片段着色器：进行 YUV→RGB 转换
    m_shader = std::make_unique<Shader>();
    if (!m_shader->loadFromFile("shaders/video.vert", "shaders/video.frag")) {
        LOG_ERROR("Failed to load video shaders");
        return false;
    }

    // 步骤2：设置纹理采样器 uniform
    // texY, texU, texV 对应三个纹理单元（GL_TEXTURE0/1/2）
    m_shader->use();
    m_shader->setInt("texY", 0);  // Y 平面绑定到纹理单元 0
    m_shader->setInt("texU", 1);  // U 平面绑定到纹理单元 1（NV12 模式下为 UV 纹理）
    m_shader->setInt("texV", 2);  // V 平面绑定到纹理单元 2
    m_shader->setInt("isNV12", 0);  // 默认 YUV420P 模式
    m_shader->unuse();
    LOG_DEBUG("Shader uniforms set successfully");

    // 步骤3：创建全屏四边形（用于显示视频）
    setupQuad();

    // 步骤4：创建 YUV 三个纹理对象
    glGenTextures(1, &m_textureY);
    glGenTextures(1, &m_textureU);
    glGenTextures(1, &m_textureV);
    LOG_DEBUG("Created YUV textures: Y=" + std::to_string(m_textureY) +
             ", U=" + std::to_string(m_textureU) +
             ", V=" + std::to_string(m_textureV));

    // 步骤5：配置 Y 纹理（亮度平面，全分辨率）
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // 缩小时线性插值
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);  // 放大时线性插值
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  // 水平方向边缘夹紧
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);  // 垂直方向边缘夹紧
    // 分配纹理内存（单通道红色，8位无符号整数）
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth, m_videoHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    LOG_DEBUG("Y texture initialized: " + std::to_string(m_videoWidth) + "x" + std::to_string(m_videoHeight));

    // 步骤6：配置 U 纹理（色度U平面，1/4 分辨率）
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // YUV420P 的 U/V 平面宽高各为 Y 平面的一半
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    LOG_DEBUG("U texture initialized: " + std::to_string(m_videoWidth / 2) + "x" + std::to_string(m_videoHeight / 2));

    // 步骤7：配置 V 纹理（色度V平面，1/4 分辨率）
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    LOG_DEBUG("V texture initialized: " + std::to_string(m_videoWidth / 2) + "x" + std::to_string(m_videoHeight / 2));

    // 步骤8：配置 NV12 UV 纹理（硬件解码零拷贝用，GL_RG8 双通道）
    glGenTextures(1, &m_textureUV);
    glBindTexture(GL_TEXTURE_2D, m_textureUV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    LOG_DEBUG("NV12 UV texture initialized: " + std::to_string(m_videoWidth / 2) + "x" + std::to_string(m_videoHeight / 2));

    glBindTexture(GL_TEXTURE_2D, 0);

    LOG_INFO("GLRenderer initialized successfully");
    return true;
}

/**
 * 销毁渲染器，释放 OpenGL 资源
 */
void GLRenderer::destroy() {
    LOG_DEBUG("Destroying GLRenderer resources");

    if (m_VAO) {
        glDeleteVertexArrays(1, &m_VAO);
        m_VAO = 0;
    }
    if (m_VBO) {
        glDeleteBuffers(1, &m_VBO);
        m_VBO = 0;
    }
    if (m_textureY) {
        glDeleteTextures(1, &m_textureY);
        m_textureY = 0;
    }
    if (m_textureU) {
        glDeleteTextures(1, &m_textureU);
        m_textureU = 0;
    }
    if (m_textureV) {
        glDeleteTextures(1, &m_textureV);
        m_textureV = 0;
    }
    if (m_textureUV) {
        glDeleteTextures(1, &m_textureUV);
        m_textureUV = 0;
    }

    LOG_DEBUG("GLRenderer resources destroyed");
}

/**
 * 渲染一帧视频数据（支持 YUV420P 和 NV12 两种格式）
 * @param yData Y 平面数据指针
 * @param uData YUV420P: U平面数据指针 / NV12: UV交错平面数据指针
 * @param vData YUV420P: V平面数据指针 / NV12: 不使用
 * @param yPitch Y 平面行跨度（字节数）
 * @param uPitch YUV420P: U平面行跨度 / NV12: UV平面行跨度
 * @param vPitch YUV420P: V平面行跨度 / NV12: 不使用
 * @param isNV12 true = NV12 格式（硬件解码输出），false = YUV420P（软件解码输出）
 */
void GLRenderer::renderFrame(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                              int yPitch, int uPitch, int vPitch, bool isNV12) {
    // 步骤1：根据像素格式选择对应的纹理上传路径
    if (isNV12) {
        updateNV12Textures(yData, uData, yPitch, uPitch);
    } else {
        updateYUVTextures(yData, uData, vData, yPitch, uPitch, vPitch);
    }

    // 步骤2：绑定纹理到对应的纹理单元
    // NV12 解交错模式下数据已拆分到 U/V 纹理，与 YUV420P 一致
    // NV12 GL_RG8 模式下 UV 数据在 m_textureUV 中，需要 shader 区分采样方式
    bool useNV12Shader = isNV12 && !m_nv12Deinterleave;

    glActiveTexture(GL_TEXTURE0);  // 纹理单元 0：Y 平面（亮度）
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glActiveTexture(GL_TEXTURE1);  // 纹理单元 1：U 平面 或 NV12 UV 纹理
    glBindTexture(GL_TEXTURE_2D, useNV12Shader ? m_textureUV : m_textureU);
    glActiveTexture(GL_TEXTURE2);  // 纹理单元 2：V 平面（仅 YUV420P / 解交错模式使用）
    glBindTexture(GL_TEXTURE_2D, m_textureV);

    // 步骤3：使用着色器程序并绘制全屏四边形
    // 片段着色器对每个像素进行 YUV→RGB (BT.709) 转换
    m_shader->use();
    m_shader->setInt("isNV12", useNV12Shader ? 1 : 0);
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);  // 绘制 6 个顶点（2 个三角形）
    glBindVertexArray(0);
    m_shader->unuse();
}

void GLRenderer::clear(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void GLRenderer::setVideoSize(int width, int height) {
    if (m_videoWidth == width && m_videoHeight == height) {
        return;
    }

    m_videoWidth = width;
    m_videoHeight = height;

    // 重新创建纹理
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth, m_videoHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    // NV12 UV 纹理也需要随分辨率变化重建
    glBindTexture(GL_TEXTURE_2D, m_textureUV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
}

/**
 * 创建全屏四边形的 VAO 和 VBO
 * 用于渲染视频帧到整个窗口
 */
void GLRenderer::setupQuad() {
    LOG_DEBUG("Setting up fullscreen quad");

    // 全屏四边形顶点数据（NDC坐标系：-1到1）
    // 每个顶点包含：位置(x,y,z) + 纹理坐标(u,v)
    float vertices[] = {
        // 位置(NDC)         // 纹理坐标
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,  // 左上角
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,  // 左下角
         1.0f, -1.0f, 0.0f,  1.0f, 1.0f,  // 右下角

        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,  // 左上角
         1.0f, -1.0f, 0.0f,  1.0f, 1.0f,  // 右下角
         1.0f,  1.0f, 0.0f,  1.0f, 0.0f   // 右上角
    };

    // 创建 VAO 和 VBO
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);

    glBindVertexArray(m_VAO);

    // 将顶点数据上传到 GPU
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 配置顶点属性 0：位置（3 个 float）
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // 配置顶点属性 1：纹理坐标（2 个 float）
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    LOG_DEBUG("Fullscreen quad created successfully");
}

/**
 * 更新 YUV 纹理数据
 * @param yData, uData, vData YUV 三个平面的数据指针
 * @param yPitch, uPitch, vPitch 各平面的行跨度（linesize）
 */
void GLRenderer::updateYUVTextures(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                                   int yPitch, int uPitch, int vPitch) {
    // 始终显式设置像素存储参数，防止外部代码（如 ImGui）修改后未恢复
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 更新 Y 纹理（亮度平面）
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, yPitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_videoWidth, m_videoHeight, GL_RED, GL_UNSIGNED_BYTE, yData);

    // 更新 U 纹理（色度U平面）
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, uPitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_videoWidth / 2, m_videoHeight / 2, GL_RED, GL_UNSIGNED_BYTE, uData);

    // 更新 V 纹理（色度V平面）
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, vPitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_videoWidth / 2, m_videoHeight / 2, GL_RED, GL_UNSIGNED_BYTE, vData);

    // 恢复默认值
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOG_DEBUG("YUV textures updated");
}

/**
 * 更新 NV12 纹理数据（硬件解码路径）
 * @param yData  Y 平面数据指针（全分辨率亮度）
 * @param uvData UV 交错平面数据指针（半分辨率，每像素 2 字节：U + V）
 * @param yPitch  Y 平面行跨度（字节数）
 * @param uvPitch UV 平面行跨度（字节数）
 *
 * NV12 格式内存布局：
 * Y 平面：  [Y0][Y1][Y2][Y3]...  每行 width 字节
 * UV 平面： [U0][V0][U1][V1]...  每行 width 字节（U/V 交错存储）
 *
 * 支持两种上传策略（由 m_nv12Deinterleave 控制）：
 * - GL_RG8 零拷贝：直接上传交错 UV 到双通道纹理（D3D11VA/DXVA2/VideoToolbox）
 * - UV 解交错：拆分为独立 U/V 后上传到单通道纹理（CUDA 兼容模式）
 */
void GLRenderer::updateNV12Textures(uint8_t* yData, uint8_t* uvData,
                                     int yPitch, int uvPitch) {
    // 始终显式设置像素存储参数，防止外部代码修改后未恢复
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // ===== Y 平面上传（两种模式共用） =====
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, yPitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_videoWidth, m_videoHeight,
                    GL_RED, GL_UNSIGNED_BYTE, yData);

    // ===== UV 平面上传（根据模式选择策略） =====
    if (m_nv12Deinterleave) {
        // 解交错模式：将 [U0,V0,U1,V1,...] 拆分为独立 U/V 平面
        // 用于 CUDA 等 GL_RG8 纹理存在兼容性问题的后端
        const int chromaWidth  = m_videoWidth  / 2;
        const int chromaHeight = m_videoHeight / 2;
        const size_t chromaSize = static_cast<size_t>(chromaWidth) * chromaHeight;

        // 按需分配解交错缓冲区（仅首次或分辨率变化时分配）
        if (m_nv12UBuffer.size() != chromaSize) {
            m_nv12UBuffer.resize(chromaSize);
            m_nv12VBuffer.resize(chromaSize);
        }

        for (int row = 0; row < chromaHeight; row++) {
            const uint8_t* src = uvData + row * uvPitch;
            uint8_t* dstU = m_nv12UBuffer.data() + row * chromaWidth;
            uint8_t* dstV = m_nv12VBuffer.data() + row * chromaWidth;
            for (int col = 0; col < chromaWidth; col++) {
                dstU[col] = src[col * 2];      // 偶数字节 → U (Cb)
                dstV[col] = src[col * 2 + 1];  // 奇数字节 → V (Cr)
            }
        }

        // 上传到独立的 U/V 纹理（与 YUV420P 相同的 GL_RED 纹理）
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);  // 解交错后无行填充
        glBindTexture(GL_TEXTURE_2D, m_textureU);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight,
                        GL_RED, GL_UNSIGNED_BYTE, m_nv12UBuffer.data());
        glBindTexture(GL_TEXTURE_2D, m_textureV);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, chromaWidth, chromaHeight,
                        GL_RED, GL_UNSIGNED_BYTE, m_nv12VBuffer.data());
    } else {
        // GL_RG8 零拷贝模式：直接上传交错 UV 到双通道纹理
        // 用于 D3D11VA/DXVA2/VideoToolbox 等 GL_RG8 兼容的后端
        glBindTexture(GL_TEXTURE_2D, m_textureUV);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, uvPitch / 2);  // GL_RG: 2 bytes/pixel
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_videoWidth / 2, m_videoHeight / 2,
                        GL_RG, GL_UNSIGNED_BYTE, uvData);
    }

    // 恢复默认值
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOG_DEBUG("NV12 textures updated");
}

} // namespace FluxPlayer
