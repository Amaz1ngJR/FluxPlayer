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
#include <glad/glad.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#endif

namespace FluxPlayer {

namespace {

static std::string getExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    return p.substr(0, p.find_last_of("\\/"));
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = PATH_MAX;
    if (_NSGetExecutablePath(buf, &size) != 0) return ".";
    std::string p(buf);
    return p.substr(0, p.find_last_of('/'));
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
    if (n <= 0) return ".";
    std::string p(buf, n);
    return p.substr(0, p.find_last_of('/'));
#endif
}

inline bool checkGLError(const char* label) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOG_ERROR(std::string(label) + " GL error: 0x" +
                  ([](GLenum e) {
                      char buf[16]; snprintf(buf, sizeof(buf), "%04X", e); return std::string(buf);
                  })(err));
        return false;
    }
    return true;
}
} // anonymous namespace

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
    std::string exeDir = getExeDir();
    if (!m_shader->loadFromFile(exeDir + "/shaders/video.vert", exeDir + "/shaders/video.frag")) {
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
    m_shader->setInt("colorSpace", 0);  // 默认 BT.601
    m_shader->setInt("fullRange", 0);   // 默认 TV/limited range
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
    if (!checkGLError("Y texture glTexImage2D")) return false;
    LOG_DEBUG("Y texture initialized: " + std::to_string(m_videoWidth) + "x" + std::to_string(m_videoHeight));

    // 步骤6：配置 U 纹理（色度U平面，1/4 分辨率）
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // YUV420P 的 U/V 平面宽高各为 Y 平面的一半
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    if (!checkGLError("U texture glTexImage2D")) return false;
    LOG_DEBUG("U texture initialized: " + std::to_string(m_videoWidth / 2) + "x" + std::to_string(m_videoHeight / 2));

    // 步骤7：配置 V 纹理（色度V平面，1/4 分辨率）
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    if (!checkGLError("V texture glTexImage2D")) return false;
    LOG_DEBUG("V texture initialized: " + std::to_string(m_videoWidth / 2) + "x" + std::to_string(m_videoHeight / 2));

    // 步骤8：配置 CPU 内存 NV12 的 UV 上传纹理（GL_RG8 双通道）
    glGenTextures(1, &m_textureUV);
    glBindTexture(GL_TEXTURE_2D, m_textureUV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    if (!checkGLError("NV12 UV texture glTexImage2D")) return false;
    LOG_DEBUG("NV12 UV texture initialized: " + std::to_string(m_videoWidth / 2) + "x" + std::to_string(m_videoHeight / 2));

    glBindTexture(GL_TEXTURE_2D, 0);

    LOG_INFO("GLRenderer initialized successfully");
    return true;
}

bool GLRenderer::initHardwareInterop(AVHWDeviceType deviceType,
                                     AVBufferRef* hwDeviceContext) {
    if (deviceType == AV_HWDEVICE_TYPE_NONE) {
        return true;
    }

    auto interop = createHardwareFrameInterop();
    if (!interop ||
        !interop->initialize(deviceType, hwDeviceContext, m_videoWidth, m_videoHeight)) {
        return false;
    }

    const std::string exeDir = getExeDir();
    if (deviceType == AV_HWDEVICE_TYPE_D3D11VA) {
        // D3D11 Video Processor 已经在 GPU 内完成 YUV->BGRA，OpenGL 只需
        // 使用 RGBA 直通着色器采样共享纹理。
        if (!m_rgbaShader) {
            m_rgbaShader = std::make_unique<Shader>();
            if (!m_rgbaShader->loadFromFile(exeDir + "/shaders/image.vert",
                                             exeDir + "/shaders/image.frag")) {
                LOG_ERROR("Failed to load D3D11 interop RGBA shader");
                return false;
            }
            m_rgbaShader->use();
            m_rgbaShader->setInt("texRGBA", 0);
            m_rgbaShader->unuse();
        }
    } else if (deviceType == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
        // CGLTexImageIOSurface2D 导出的 plane 是 GL_TEXTURE_RECTANGLE，
        // 必须使用 sampler2DRect，不能套用普通 sampler2D 着色器。
        m_hardwareYuvShader = std::make_unique<Shader>();
        if (!m_hardwareYuvShader->loadFromFile(exeDir + "/shaders/video.vert",
                                                exeDir + "/shaders/video_rect.frag")) {
            LOG_ERROR("Failed to load VideoToolbox IOSurface shader");
            m_hardwareYuvShader.reset();
            return false;
        }
        m_hardwareYuvShader->use();
        m_hardwareYuvShader->setInt("texY", 0);
        m_hardwareYuvShader->setInt("texUV", 1);
        m_hardwareYuvShader->unuse();
    }

    m_hardwareInterop = std::move(interop);
    return true;
}

/**
 * 销毁渲染器，释放 OpenGL 资源
 */
void GLRenderer::destroy() {
    LOG_DEBUG("Destroying GLRenderer resources");

    // 平台互操作对象持有 WGL 注册对象、IOSurface AVFrame 引用和 GL fence，
    // 必须在普通纹理及当前 OpenGL 上下文销毁前先释放。
    m_hardwareInterop.reset();
    m_hardwareYuvShader.reset();
    m_lastFrameWasHardware = false;

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
    if (m_textureRGBA) {
        glDeleteTextures(1, &m_textureRGBA);
        m_textureRGBA = 0;
    }

    LOG_DEBUG("GLRenderer resources destroyed");
}

bool GLRenderer::renderFrame(const AVFrame* frame, int colorSpace, int fullRange) {
    if (!frame) {
        return false;
    }

    const auto format = static_cast<AVPixelFormat>(frame->format);
    const bool isHardwareFrame =
        format == AV_PIX_FMT_D3D11 || format == AV_PIX_FMT_VIDEOTOOLBOX;

    if (isHardwareFrame) {
        if (!m_hardwareInterop) {
            LOG_ERROR("Received a hardware frame without an initialized zero-copy interop");
            return false;
        }

        HardwareTextureBinding binding;
        if (!m_hardwareInterop->beginFrame(frame, binding)) {
            // 这里不允许调用 av_hwframe_transfer_data 作为隐藏回退。若平台链路
            // 无法建立，初始化阶段就应切换为软件解码。
            LOG_ERROR("Failed to import hardware frame without CPU copy");
            return false;
        }

        drawHardwareBinding(binding, colorSpace, fullRange);
        m_hardwareInterop->endFrame();

        m_hasValidTexture = true;
        m_lastFrameWasHardware = true;
        m_lastColorSpace = colorSpace;
        m_lastFullRange = fullRange;
        return true;
    }

    if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_NV12) {
        LOG_ERROR("Renderer received unsupported software pixel format: " +
                  std::to_string(frame->format));
        return false;
    }

    const bool isNV12 = format == AV_PIX_FMT_NV12;
    renderFrame(
        const_cast<uint8_t*>(frame->data[0]),
        const_cast<uint8_t*>(frame->data[1]),
        const_cast<uint8_t*>(frame->data[2]),
        frame->linesize[0],
        frame->linesize[1],
        frame->linesize[2],
        isNV12,
        colorSpace,
        fullRange);
    return true;
}

/**
 * 渲染一帧视频数据（支持 YUV420P 和 NV12 两种格式）
 * @param yData Y 平面数据指针
 * @param uData YUV420P: U平面数据指针 / NV12: UV交错平面数据指针
 * @param vData YUV420P: V平面数据指针 / NV12: 不使用
 * @param yPitch Y 平面行跨度（字节数）
 * @param uPitch YUV420P: U平面行跨度 / NV12: UV平面行跨度
 * @param vPitch YUV420P: V平面行跨度 / NV12: 不使用
 * @param isNV12 true = CPU 内存 NV12，false = CPU 内存 YUV420P
 */
void GLRenderer::renderFrame(uint8_t* yData, uint8_t* uData, uint8_t* vData,
                              int yPitch, int uPitch, int vPitch,
                              bool isNV12, int colorSpace, int fullRange) {
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
    m_shader->setInt("colorSpace", colorSpace);
    m_shader->setInt("fullRange", fullRange);
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);  // 绘制 6 个顶点（2 个三角形）
    glBindVertexArray(0);
    m_shader->unuse();

    // 纹理数据已上传，标记 GPU 侧帧有效（暂停时可复用）
    m_hasValidTexture = true;
    m_lastIsNV12Shader = useNV12Shader;
    m_lastColorSpace = colorSpace;
    m_lastFullRange = fullRange;
    m_lastFrameWasHardware = false;
}

void GLRenderer::drawHardwareBinding(const HardwareTextureBinding& binding,
                                     int colorSpace,
                                     int fullRange) {
    if (binding.layout == HardwareTextureLayout::RGBA2D) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(binding.textureTarget, binding.textureRGBA);
        m_rgbaShader->use();
        glBindVertexArray(m_VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        m_rgbaShader->unuse();
        glBindTexture(binding.textureTarget, 0);
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(binding.textureTarget, binding.textureY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(binding.textureTarget, binding.textureUV);

    m_hardwareYuvShader->use();
    m_hardwareYuvShader->setInt("colorSpace", colorSpace);
    m_hardwareYuvShader->setInt("fullRange", fullRange);
    m_hardwareYuvShader->setVec2(
        "textureSizeY",
        static_cast<float>(binding.width),
        static_cast<float>(binding.height));
    m_hardwareYuvShader->setVec2(
        "textureSizeUV",
        static_cast<float>(binding.uvWidth),
        static_cast<float>(binding.uvHeight));
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    m_hardwareYuvShader->unuse();

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(binding.textureTarget, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(binding.textureTarget, 0);
}

/**
 * 使用 GPU 纹理中已有的帧数据重新渲染
 * 纯音频模式（m_staticImageUploaded）使用 RGBA 着色器渲染封面图；
 * 视频模式使用 YUV 着色器渲染上一帧
 */
void GLRenderer::renderCachedFrame() {
    if (!m_hasValidTexture) return;

    if (m_lastFrameWasHardware && m_hardwareInterop) {
        HardwareTextureBinding binding;
        if (m_hardwareInterop->beginCachedFrame(binding)) {
            drawHardwareBinding(binding, m_lastColorSpace, m_lastFullRange);
            m_hardwareInterop->endFrame();
        }
        return;
    }

    // 纯音频模式：使用 RGBA 直通着色器渲染封面图
    if (m_staticImageUploaded && m_rgbaShader) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_textureRGBA);
        m_rgbaShader->use();
        glBindVertexArray(m_VAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        m_rgbaShader->unuse();
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_lastIsNV12Shader ? m_textureUV : m_textureU);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);

    m_shader->use();
    m_shader->setInt("isNV12", m_lastIsNV12Shader ? 1 : 0);
    m_shader->setInt("colorSpace", m_lastColorSpace);
    m_shader->setInt("fullRange", m_lastFullRange);
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    m_shader->unuse();
}

/**
 * 渲染静态 RGBA 图片（纯音频模式封面图）
 * 首次调用时创建纹理并上传数据；后续调用直接复用已上传的纹理
 */
void GLRenderer::renderStaticImage(const uint8_t* rgbaData, int width, int height) {
    // 首次调用：加载 RGBA 着色器并创建纹理
    if (!m_staticImageUploaded) {
        // 加载 RGBA 直通着色器
        if (!m_rgbaShader) {
            m_rgbaShader = std::make_unique<Shader>();
            std::string exeDir = getExeDir();
            if (!m_rgbaShader->loadFromFile(exeDir + "/shaders/image.vert",
                                             exeDir + "/shaders/image.frag")) {
                LOG_ERROR("Failed to load image shaders");
                m_rgbaShader.reset();
                return;
            }
            m_rgbaShader->use();
            m_rgbaShader->setInt("texRGBA", 0);  // RGBA 纹理绑定到纹理单元 0
            m_rgbaShader->unuse();
        }

        // 创建 RGBA 纹理
        glGenTextures(1, &m_textureRGBA);
        glBindTexture(GL_TEXTURE_2D, m_textureRGBA);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);
        glBindTexture(GL_TEXTURE_2D, 0);

        m_staticImageUploaded = true;
        m_hasValidTexture = true;
        m_lastFrameWasHardware = false;
        LOG_INFO("Static RGBA image uploaded: " + std::to_string(width) + "x" + std::to_string(height));
    }

    // 渲染（首次或后续调用均执行）
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureRGBA);
    m_rgbaShader->use();
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    m_rgbaShader->unuse();
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
    m_hasValidTexture = false;  // 分辨率变化，纹理数据失效

    // 重新创建纹理
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth, m_videoHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    checkGLError("setVideoSize Y texture glTexImage2D");

    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    checkGLError("setVideoSize U texture glTexImage2D");

    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    checkGLError("setVideoSize V texture glTexImage2D");

    // NV12 UV 纹理也需要随分辨率变化重建
    glBindTexture(GL_TEXTURE_2D, m_textureUV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, m_videoWidth / 2, m_videoHeight / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
    checkGLError("setVideoSize NV12 UV texture glTexImage2D");

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
 * 更新位于 CPU 内存中的 NV12 纹理数据
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
 * - GL_RG8 直接上传：把交错 UV 上传到双通道纹理
 * - UV 解交错：拆分为独立 U/V 后上传到单通道纹理（旧驱动兼容模式）
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
        // 仅用于 GL_RG8 纹理存在兼容性问题的旧驱动
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
        // GL_RG8 直接上传模式：避免 CPU 解交错，但仍会发生一次 CPU->GPU 上传。
        // 真正的硬件帧不会进入本函数，而由 HardwareFrameInterop 导入原生 surface。
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
