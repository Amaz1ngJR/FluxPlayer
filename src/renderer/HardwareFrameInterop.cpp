/**
 * @file HardwareFrameInterop.cpp
 * @brief Windows D3D11VA 与 macOS VideoToolbox 的零 CPU 拷贝 OpenGL 互操作
 */

#include "FluxPlayer/renderer/HardwareFrameInterop.h"
#include "FluxPlayer/utils/Logger.h"

#include <glad/glad.h>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#if defined(_WIN32)

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#elif defined(__APPLE__)

#define COREVIDEO_SILENCE_GL_DEPRECATION
#define GL_SILENCE_DEPRECATION

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>

extern "C" {
#include <libavutil/hwcontext_videotoolbox.h>
}

#endif

namespace FluxPlayer {

namespace {

#if defined(_WIN32)

// WGL_NV_DX_interop2 没有随 MinGW 的基础 OpenGL 头稳定提供，因此在运行时加载。
// 这些签名和常量来自扩展规范；使用 void* 接收 D3D COM 对象可避免依赖厂商头文件。
constexpr GLenum kWglAccessReadOnlyNV = 0x0000;

using WglDXOpenDeviceNVProc = HANDLE(WINAPI*)(void* dxDevice);
using WglDXCloseDeviceNVProc = BOOL(WINAPI*)(HANDLE dxDevice);
using WglDXRegisterObjectNVProc = HANDLE(WINAPI*)(HANDLE dxDevice,
                                                  void* dxObject,
                                                  GLuint name,
                                                  GLenum type,
                                                  GLenum access);
using WglDXUnregisterObjectNVProc = BOOL(WINAPI*)(HANDLE dxDevice, HANDLE object);
using WglDXLockObjectsNVProc = BOOL(WINAPI*)(HANDLE dxDevice, GLint count, HANDLE* objects);
using WglDXUnlockObjectsNVProc = BOOL(WINAPI*)(HANDLE dxDevice, GLint count, HANDLE* objects);

template <typename T>
void releaseCom(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

std::string hresultToString(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<unsigned long>(result);
    return stream.str();
}

/**
 * @brief 将 Windows 宽字符串转换为 UTF-8，保证显卡名称可直接交给 ImGui 显示
 */
std::string wideStringToUtf8(const wchar_t* text) {
    if (!text || !text[0]) {
        return {};
    }

    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }

    // length 包含结尾的 '\0'。先为它保留空间，转换完成后再移除终止符，
    // 避免向只分配 length - 1 字节的 std::string 尾部越界写入。
    std::string utf8(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, 0, text, -1, utf8.data(), length, nullptr, nullptr) <= 0) {
        return {};
    }
    utf8.pop_back();
    return utf8;
}

/**
 * @brief 从 FFmpeg 使用的 D3D11 设备反查实际 DXGI 显卡名称
 *
 * 必须查询同一个 D3D11 device，不能另行枚举“第一块显卡”，否则多显卡机器上
 * 状态面板可能显示集成显卡，而实际硬解码和 WGL 互操作运行在独立显卡上。
 */
std::string getD3D11AdapterName(ID3D11Device* device) {
    if (!device) {
        return {};
    }

    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT result = device->QueryInterface(
        IID_IDXGIDevice, reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(result)) {
        return {};
    }

    IDXGIAdapter* adapter = nullptr;
    result = dxgiDevice->GetAdapter(&adapter);
    releaseCom(dxgiDevice);
    if (FAILED(result) || !adapter) {
        return {};
    }

    DXGI_ADAPTER_DESC description{};
    result = adapter->GetDesc(&description);
    releaseCom(adapter);
    if (FAILED(result)) {
        return {};
    }
    return wideStringToUtf8(description.Description);
}

/**
 * @brief 安全读取 WGL 扩展函数
 *
 * wglGetProcAddress() 会用 1、2、3、4、-1 表示失败，不能只检查 nullptr。
 */
template <typename T>
T loadWglFunction(const char* name) {
    PROC proc = wglGetProcAddress(name);
    const auto value = reinterpret_cast<std::intptr_t>(proc);
    if (!proc || value == 1 || value == 2 || value == 3 || value == 4 || value == -1) {
        return nullptr;
    }
    return reinterpret_cast<T>(proc);
}

class D3D11OpenGLInterop final : public HardwareFrameInterop {
public:
    ~D3D11OpenGLInterop() override {
        destroy();
    }

    const char* backendName() const override {
        return "D3D11VA";
    }

    const char* deviceName() const override {
        return m_deviceName.c_str();
    }

    const char* zeroCopyMode() const override {
        return "D3D11 Video Processor -> GPU Copy -> WGL/GL";
    }

    bool initialize(AVHWDeviceType deviceType,
                    AVBufferRef* hwDeviceContext,
                    int width,
                    int height) override {
        if (deviceType != AV_HWDEVICE_TYPE_D3D11VA || !hwDeviceContext ||
            !wglGetCurrentContext()) {
            LOG_WARN("D3D11/OpenGL interop initialization parameters are invalid");
            return false;
        }
        m_glContext = wglGetCurrentContext();

        m_openDevice = loadWglFunction<WglDXOpenDeviceNVProc>("wglDXOpenDeviceNV");
        m_closeDevice = loadWglFunction<WglDXCloseDeviceNVProc>("wglDXCloseDeviceNV");
        m_registerObject = loadWglFunction<WglDXRegisterObjectNVProc>("wglDXRegisterObjectNV");
        m_unregisterObject = loadWglFunction<WglDXUnregisterObjectNVProc>("wglDXUnregisterObjectNV");
        m_lockObjects = loadWglFunction<WglDXLockObjectsNVProc>("wglDXLockObjectsNV");
        m_unlockObjects = loadWglFunction<WglDXUnlockObjectsNVProc>("wglDXUnlockObjectsNV");

        if (!m_openDevice || !m_closeDevice || !m_registerObject ||
            !m_unregisterObject || !m_lockObjects || !m_unlockObjects) {
            LOG_WARN("WGL_NV_DX_interop2 is unavailable; zero-copy D3D11 rendering is disabled");
            return false;
        }

        auto* avDevice = reinterpret_cast<AVHWDeviceContext*>(hwDeviceContext->data);
        auto* d3dDeviceContext =
            avDevice ? reinterpret_cast<AVD3D11VADeviceContext*>(avDevice->hwctx) : nullptr;
        if (!d3dDeviceContext || !d3dDeviceContext->device) {
            LOG_WARN("FFmpeg D3D11 device context is unavailable");
            return false;
        }

        // FFmpeg 的解码线程也会访问同一个 immediate/video context。
        // 复用 hwcontext_d3d11va 提供的递归锁，确保解码提交和 VideoProcessorBlt
        // 不会跨线程同时修改 D3D11 context 状态。
        m_deviceLock = d3dDeviceContext->lock;
        m_deviceUnlock = d3dDeviceContext->unlock;
        m_deviceLockContext = d3dDeviceContext->lock_ctx;

        m_device = d3dDeviceContext->device;
        m_device->AddRef();
        m_device->GetImmediateContext(&m_deviceContext);
        m_deviceName = getD3D11AdapterName(m_device);
        if (m_deviceName.empty()) {
            m_deviceName = "D3D11 GPU";
        }
        LOG_INFO("D3D11 hardware adapter: " + m_deviceName);

        HRESULT result = m_device->QueryInterface(
            IID_ID3D11VideoDevice, reinterpret_cast<void**>(&m_videoDevice));
        if (FAILED(result)) {
            LOG_WARN("ID3D11VideoDevice is unavailable: " + hresultToString(result));
            destroy();
            return false;
        }

        result = m_deviceContext->QueryInterface(
            IID_ID3D11VideoContext, reinterpret_cast<void**>(&m_videoContext));
        if (FAILED(result)) {
            LOG_WARN("ID3D11VideoContext is unavailable: " + hresultToString(result));
            destroy();
            return false;
        }

        m_wglDevice = m_openDevice(m_device);
        if (!m_wglDevice) {
            LOG_WARN("OpenGL driver rejected the FFmpeg D3D11 device");
            destroy();
            return false;
        }

        // 本地文件通常在初始化解码器时已经知道画面尺寸，可以立即创建共享纹理。
        // RTSP 等网络输入有时要等第一帧才补齐宽高；此时只初始化 D3D11/WGL
        // 设备互操作，输出纹理会在 beginFrame() 收到有效尺寸后创建。这个延迟创建
        // 仅推迟 GPU 资源分配，不会引入 CPU 像素缓冲或硬件帧下载。
        if (width > 0 && height > 0) {
            if (!createOutputResources(width, height)) {
                destroy();
                return false;
            }
        } else {
            LOG_INFO("D3D11 interop output texture creation deferred until the first frame");
        }

        LOG_INFO("D3D11VA zero-copy path enabled: decoder surface -> "
                 "D3D11 Video Processor -> GPU copy -> WGL renderbuffer -> OpenGL texture");
        return true;
    }

    bool beginFrame(const AVFrame* frame, HardwareTextureBinding& binding) override {
        if (!frame || frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) {
            return false;
        }

        auto* inputTexture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        const UINT arraySlice =
            static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame->data[1]));

        ID3D11Device* frameDevice = nullptr;
        inputTexture->GetDevice(&frameDevice);
        const bool sameDevice = frameDevice == m_device;
        releaseCom(frameDevice);
        if (!sameDevice) {
            LOG_ERROR("D3D11 frame belongs to a different device than the WGL interop device");
            return false;
        }

        if (frame->width <= 0 || frame->height <= 0) {
            return false;
        }
        if (frame->width != m_width || frame->height != m_height) {
            if (!createOutputResources(frame->width, frame->height)) {
                return false;
            }
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = arraySlice;

        ID3D11VideoProcessorInputView* inputView = nullptr;
        HRESULT result = m_videoDevice->CreateVideoProcessorInputView(
            inputTexture, m_enumerator, &inputDesc, &inputView);
        if (FAILED(result)) {
            LOG_ERROR("Failed to create D3D11 video input view: " + hresultToString(result));
            return false;
        }

        const RECT sourceRect{0, 0, frame->width, frame->height};
        const RECT targetRect{0, 0, m_width, m_height};
        if (m_deviceLock) {
            m_deviceLock(m_deviceLockContext);
        }

        m_videoContext->VideoProcessorSetStreamFrameFormat(
            m_processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_videoContext->VideoProcessorSetStreamSourceRect(m_processor, 0, TRUE, &sourceRect);
        m_videoContext->VideoProcessorSetStreamDestRect(m_processor, 0, TRUE, &targetRect);
        m_videoContext->VideoProcessorSetOutputTargetRect(m_processor, TRUE, &targetRect);
        m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor, 0, FALSE);
        m_videoContext->VideoProcessorSetOutputAlphaFillMode(
            m_processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);

        // Video Processor 在 GPU 内完成 YUV->RGBA。色彩空间元数据通过 D3D11
        // 传入，OpenGL 侧只做 RGBA 采样，避免再次转换或读取像素。
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
        inputColor.YCbCr_Matrix =
            (frame->colorspace == AVCOL_SPC_BT709 ||
             frame->colorspace == AVCOL_SPC_BT2020_NCL ||
             frame->colorspace == AVCOL_SPC_BT2020_CL) ? 1 : 0;
        inputColor.Nominal_Range =
            frame->color_range == AVCOL_RANGE_JPEG ? 2 : 1;
        m_videoContext->VideoProcessorSetStreamColorSpace(
            m_processor, 0, &inputColor);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
        outputColor.RGB_Range = 0;
        outputColor.Nominal_Range = 2;
        m_videoContext->VideoProcessorSetOutputColorSpace(m_processor, &outputColor);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.InputFrameOrField = m_frameIndex;
        stream.pInputSurface = inputView;
        result = m_videoContext->VideoProcessorBlt(
            m_processor, m_outputView, m_frameIndex, 1, &stream);
        ++m_frameIndex;

        // VideoProcessor 运行在视频引擎上。不要把它的输出纹理直接注册给 WGL：
        // 部分 NVIDIA 驱动无法正确同步“视频引擎输出 + OpenGL 采样”，表现为黑屏，
        // 注销资源时还可能在 nvwgf2umx.dll 内访问冲突。Blt 成功后先用 D3D11
        // graphics context 拷贝到专用互操作纹理，让 WGL 只接触普通 CopyResource
        // 目标。该拷贝完全在 GPU 内完成，不创建 staging resource，也不映射 CPU。
        if (SUCCEEDED(result)) {
            m_deviceContext->CopyResource(m_interopTexture, m_outputTexture);
        }

        // Flush 只提交 GPU 命令，不会等待或下载像素。随后 WGL lock 在驱动内部
        // 建立 D3D11 CopyResource 写入与 OpenGL framebuffer blit 之间的同步。
        m_deviceContext->Flush();
        if (m_deviceUnlock) {
            m_deviceUnlock(m_deviceLockContext);
        }

        releaseCom(inputView);
        if (FAILED(result)) {
            LOG_ERROR("D3D11 VideoProcessorBlt failed: " + hresultToString(result));
            return false;
        }

        if (!copyInteropRenderbufferToTexture()) {
            return false;
        }

        binding.layout = HardwareTextureLayout::RGBA2D;
        binding.textureTarget = GL_TEXTURE_2D;
        binding.textureRGBA = m_glTexture;
        binding.textureY = 0;
        binding.textureUV = 0;
        binding.width = m_width;
        binding.height = m_height;
        binding.uvWidth = 0;
        binding.uvHeight = 0;
        return true;
    }

    bool beginCachedFrame(HardwareTextureBinding& binding) override {
        if (!m_hasFrame) {
            return false;
        }

        // 每个新帧已经在 beginFrame() 中从共享 renderbuffer 拷贝到普通 GL 纹理。
        // 暂停或等待下一帧时直接复用该纹理，不再重新锁定 D3D11 资源。
        binding.layout = HardwareTextureLayout::RGBA2D;
        binding.textureTarget = GL_TEXTURE_2D;
        binding.textureRGBA = m_glTexture;
        binding.textureY = 0;
        binding.textureUV = 0;
        binding.width = m_width;
        binding.height = m_height;
        binding.uvWidth = 0;
        binding.uvHeight = 0;
        return true;
    }

    void endFrame() override {
        // Windows 路径在 beginFrame() 内完成 WGL lock、GPU blit 和 unlock。
        // 返回给 GLRenderer 的是普通 OpenGL 纹理，因此绘制结束时无需再接触
        // D3D11/WGL 对象，也不会让共享资源跨越 ImGui 或 swapBuffers 生命周期。
    }

private:
    bool createOutputResources(int width, int height) {
        releaseOutputResources();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        // 某些驱动会接受 0/0 帧率的 enumerator，却在 VideoProcessorBlt 时持续
        // 输出黑帧。这里使用稳定的名义帧率；当前只做逐帧颜色转换/缩放，不依赖
        // Video Processor 的帧率转换，所以该值不会改变播放器的实际播放速度。
        contentDesc.InputFrameRate.Numerator = 60;
        contentDesc.InputFrameRate.Denominator = 1;
        contentDesc.InputWidth = static_cast<UINT>(width);
        contentDesc.InputHeight = static_cast<UINT>(height);
        contentDesc.OutputFrameRate.Numerator = 60;
        contentDesc.OutputFrameRate.Denominator = 1;
        contentDesc.OutputWidth = static_cast<UINT>(width);
        contentDesc.OutputHeight = static_cast<UINT>(height);
        contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT result =
            m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_enumerator);
        if (FAILED(result)) {
            LOG_WARN("Failed to create D3D11 video processor enumerator: " +
                     hresultToString(result));
            return false;
        }

        UINT formatFlags = 0;
        result = m_enumerator->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &formatFlags);
        if (FAILED(result) ||
            !(formatFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            LOG_WARN("D3D11 video processor cannot output BGRA8");
            releaseOutputResources();
            return false;
        }

        result = m_videoDevice->CreateVideoProcessor(m_enumerator, 0, &m_processor);
        if (FAILED(result)) {
            LOG_WARN("Failed to create D3D11 video processor: " +
                     hresultToString(result));
            releaseOutputResources();
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        result = m_device->CreateTexture2D(&textureDesc, nullptr, &m_outputTexture);
        if (FAILED(result)) {
            LOG_WARN("Failed to create D3D11 interop output texture: " +
                     hresultToString(result));
            releaseOutputResources();
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = 0;
        result = m_videoDevice->CreateVideoProcessorOutputView(
            m_outputTexture, m_enumerator, &outputDesc, &m_outputView);
        if (FAILED(result)) {
            LOG_WARN("Failed to create D3D11 video output view: " +
                     hresultToString(result));
            releaseOutputResources();
            return false;
        }

        // 互操作纹理与 VideoProcessor 输出纹理格式/尺寸一致，但不挂接 output view。
        // 它只作为 CopyResource 目标以及 WGL 共享源，隔离 D3D11 视频引擎状态。
        D3D11_TEXTURE2D_DESC interopTextureDesc = textureDesc;
        interopTextureDesc.BindFlags =
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        result = m_device->CreateTexture2D(
            &interopTextureDesc, nullptr, &m_interopTexture);
        if (FAILED(result)) {
            LOG_WARN("Failed to create isolated D3D11 interop texture: " +
                     hresultToString(result));
            releaseOutputResources();
            return false;
        }

        // D3D11 VideoProcessor 的输出是 render target。WGL_NV_DX_interop2 规范
        // 建议将这类资源注册为 GL_RENDERBUFFER：显卡控制面板可能对 render target
        // 强制应用不同的采样状态，若直接注册为 GL_TEXTURE_2D，GL 目标类型与底层
        // D3D 资源不一致时结果未定义，NVIDIA 驱动上会表现为黑屏甚至注销时崩溃。
        glGenRenderbuffers(1, &m_glInteropRenderbuffer);
        m_wglObject = m_registerObject(
            m_wglDevice,
            m_interopTexture,
            m_glInteropRenderbuffer,
            GL_RENDERBUFFER,
            kWglAccessReadOnlyNV);
        if (!m_wglObject) {
            LOG_WARN("Failed to register D3D11 output renderbuffer with OpenGL; error=" +
                     std::to_string(GetLastError()));
            releaseOutputResources();
            return false;
        }

        // 普通 GL 纹理只接收一次 GPU 内部 framebuffer blit。它没有 CPU backing，
        // 也不属于 D3D11/WGL 共享对象，后续着色器采样和暂停帧复用不会触碰驱动的
        // 跨 API 所有权状态。
        glGenTextures(1, &m_glTexture);
        glBindTexture(GL_TEXTURE_2D, m_glTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &m_interopReadFramebuffer);
        glGenFramebuffers(1, &m_textureDrawFramebuffer);

        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

        if (!lockInteropObject()) {
            releaseOutputResources();
            return false;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_interopReadFramebuffer);
        glFramebufferRenderbuffer(
            GL_READ_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_RENDERBUFFER,
            m_glInteropRenderbuffer);
        const GLenum readStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_textureDrawFramebuffer);
        glFramebufferTexture2D(
            GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            m_glTexture,
            0);
        const GLenum drawStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
        const bool unlockSucceeded = unlockInteropObject();

        if (readStatus != GL_FRAMEBUFFER_COMPLETE ||
            drawStatus != GL_FRAMEBUFFER_COMPLETE ||
            !unlockSucceeded) {
            LOG_WARN("D3D11/OpenGL interop framebuffer is incomplete");
            releaseOutputResources();
            return false;
        }

        m_width = width;
        m_height = height;
        m_hasFrame = false;
        m_frameIndex = 0;
        return true;
    }

    bool lockInteropObject() {
        if (m_locked) {
            return true;
        }
        if (!m_wglDevice || !m_wglObject ||
            !m_glContext || wglGetCurrentContext() != m_glContext) {
            return false;
        }

        HANDLE object = m_wglObject;
        if (!m_lockObjects(m_wglDevice, 1, &object)) {
            LOG_ERROR("wglDXLockObjectsNV failed; error=" +
                      std::to_string(GetLastError()));
            return false;
        }
        m_locked = true;
        return true;
    }

    bool unlockInteropObject() {
        if (!m_locked) {
            return true;
        }
        if (!m_wglDevice || !m_wglObject ||
            !m_glContext || wglGetCurrentContext() != m_glContext) {
            return false;
        }

        HANDLE object = m_wglObject;
        if (!m_unlockObjects(m_wglDevice, 1, &object)) {
            LOG_ERROR("wglDXUnlockObjectsNV failed; error=" +
                      std::to_string(GetLastError()));
            return false;
        }
        m_locked = false;
        return true;
    }

    bool copyInteropRenderbufferToTexture() {
        if (!lockInteropObject()) {
            return false;
        }

        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_interopReadFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_textureDrawFramebuffer);
        glBlitFramebuffer(
            0, 0, m_width, m_height,
            0, 0, m_width, m_height,
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST);
        const GLenum glError = glGetError();

        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);

        // unlock 是 OpenGL 与 Direct3D 的所有权/同步边界。调用成功时，驱动必须先
        // 完成前面的 framebuffer blit，之后 D3D11 才能再次写共享 renderbuffer。
        // 普通 m_glTexture 已与共享资源解耦，GLRenderer 可在 unlock 后安全采样。
        if (!unlockInteropObject()) {
            return false;
        }
        if (glError != GL_NO_ERROR) {
            LOG_ERROR("Failed to blit D3D11 interop renderbuffer to GL texture; error=" +
                      std::to_string(static_cast<unsigned int>(glError)));
            return false;
        }

        if (!m_hasFrame) {
            LOG_INFO("First D3D11 zero-copy frame copied to the OpenGL render texture");
        }
        m_hasFrame = true;
        return true;
    }

    void abandonOutputResources() {
        // 只在 WGL 对象已经无法合法注销时使用。所有成员都是原始句柄/COM 指针，
        // 清空它们会有意保留驱动引用，直到所属 GL/D3D 上下文最终销毁。停止阶段的
        // 少量资源泄漏比向驱动传递失效或仍锁定的句柄安全得多。
        m_wglObject = nullptr;
        m_wglDevice = nullptr;
        m_glTexture = 0;
        m_glInteropRenderbuffer = 0;
        m_interopReadFramebuffer = 0;
        m_textureDrawFramebuffer = 0;
        m_outputView = nullptr;
        m_outputTexture = nullptr;
        m_interopTexture = nullptr;
        m_processor = nullptr;
        m_enumerator = nullptr;
        m_locked = false;
        m_hasFrame = false;
        m_width = 0;
        m_height = 0;
        m_frameIndex = 0;
    }

    void releaseOutputResources() {
        // WGL 扩展函数和所有 GL 删除操作都只能在创建这些对象的上下文当前时调用。
        // 正常 Player::cleanup() 会先恢复该上下文；这里再次防御，避免窗口/上下文
        // 已提前销毁时把失效句柄传给显卡驱动并触发访问冲突。
        const bool hasCreationContext =
            m_glContext && wglGetCurrentContext() == m_glContext;
        if ((m_wglObject || m_glTexture || m_glInteropRenderbuffer) &&
            !hasCreationContext) {
            LOG_WARN("Skipping D3D11/WGL interop teardown because its GL context is not current");
            abandonOutputResources();
            return;
        }

        if (m_locked && !unlockInteropObject()) {
            LOG_WARN("D3D11/WGL interop object could not be unlocked during teardown");
            abandonOutputResources();
            return;
        }

        if (hasCreationContext) {
            // 停止播放时允许一次阻塞式 GPU 排空。它不在逐帧路径中，也不会回读
            // 像素；作用是保证没有 framebuffer blit/采样仍引用待注销的对象。
            LOG_INFO("Releasing D3D11/OpenGL interop resources");
            glFinish();
        }
        if (m_wglObject && m_wglDevice && m_unregisterObject) {
            if (!m_unregisterObject(m_wglDevice, m_wglObject)) {
                LOG_WARN("wglDXUnregisterObjectNV failed; error=" +
                         std::to_string(GetLastError()));
            }
            m_wglObject = nullptr;
        }
        if (m_interopReadFramebuffer) {
            glDeleteFramebuffers(1, &m_interopReadFramebuffer);
            m_interopReadFramebuffer = 0;
        }
        if (m_textureDrawFramebuffer) {
            glDeleteFramebuffers(1, &m_textureDrawFramebuffer);
            m_textureDrawFramebuffer = 0;
        }
        if (m_glInteropRenderbuffer) {
            glDeleteRenderbuffers(1, &m_glInteropRenderbuffer);
            m_glInteropRenderbuffer = 0;
        }
        if (m_glTexture) {
            glDeleteTextures(1, &m_glTexture);
            m_glTexture = 0;
        }
        releaseCom(m_outputView);
        releaseCom(m_interopTexture);
        releaseCom(m_outputTexture);
        releaseCom(m_processor);
        releaseCom(m_enumerator);
        m_hasFrame = false;
        m_width = 0;
        m_height = 0;
        m_frameIndex = 0;
        if (hasCreationContext) {
            LOG_INFO("D3D11/OpenGL interop resources released");
        }
    }

    void destroy() {
        releaseOutputResources();
        if (m_wglDevice && m_closeDevice) {
            if (m_glContext && wglGetCurrentContext() == m_glContext) {
                if (!m_closeDevice(m_wglDevice)) {
                    LOG_WARN("wglDXCloseDeviceNV failed; error=" +
                             std::to_string(GetLastError()));
                }
            } else {
                LOG_WARN("Skipping WGL interop device close because its GL context is not current");
            }
            m_wglDevice = nullptr;
        }
        releaseCom(m_videoContext);
        releaseCom(m_videoDevice);
        releaseCom(m_deviceContext);
        releaseCom(m_device);
        m_glContext = nullptr;
    }

    WglDXOpenDeviceNVProc m_openDevice = nullptr;
    WglDXCloseDeviceNVProc m_closeDevice = nullptr;
    WglDXRegisterObjectNVProc m_registerObject = nullptr;
    WglDXUnregisterObjectNVProc m_unregisterObject = nullptr;
    WglDXLockObjectsNVProc m_lockObjects = nullptr;
    WglDXUnlockObjectsNVProc m_unlockObjects = nullptr;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_deviceContext = nullptr;
    ID3D11VideoDevice* m_videoDevice = nullptr;
    ID3D11VideoContext* m_videoContext = nullptr;
    ID3D11VideoProcessorEnumerator* m_enumerator = nullptr;
    ID3D11VideoProcessor* m_processor = nullptr;
    ID3D11Texture2D* m_outputTexture = nullptr;
    ID3D11Texture2D* m_interopTexture = nullptr;
    ID3D11VideoProcessorOutputView* m_outputView = nullptr;

    HANDLE m_wglDevice = nullptr;
    HANDLE m_wglObject = nullptr;
    HGLRC m_glContext = nullptr;
    GLuint m_glInteropRenderbuffer = 0;
    GLuint m_interopReadFramebuffer = 0;
    GLuint m_textureDrawFramebuffer = 0;
    GLuint m_glTexture = 0;
    int m_width = 0;
    int m_height = 0;
    UINT m_frameIndex = 0;
    bool m_locked = false;
    bool m_hasFrame = false;
    void (*m_deviceLock)(void*) = nullptr;
    void (*m_deviceUnlock)(void*) = nullptr;
    void* m_deviceLockContext = nullptr;
    std::string m_deviceName = "D3D11 GPU";
};

#elif defined(__APPLE__)

class VideoToolboxOpenGLInterop final : public HardwareFrameInterop {
public:
    ~VideoToolboxOpenGLInterop() override {
        destroy();
    }

    const char* backendName() const override {
        return "VideoToolbox";
    }

    const char* deviceName() const override {
        return m_deviceName.c_str();
    }

    const char* zeroCopyMode() const override {
        return "IOSurface -> CGL texture -> OpenGL";
    }

    bool initialize(AVHWDeviceType deviceType,
                    AVBufferRef*,
                    int,
                    int) override {
        if (deviceType != AV_HWDEVICE_TYPE_VIDEOTOOLBOX) {
            return false;
        }

        m_context = CGLGetCurrentContext();
        if (!m_context) {
            LOG_WARN("No current CGL context for VideoToolbox zero-copy rendering");
            return false;
        }

        // VideoToolbox 本身不公开固定功能解码引擎的型号，但 IOSurface 最终由当前
        // CGL 设备直接采样。读取 GL_RENDERER 可以准确区分 Apple Silicon、Intel
        // 核显和 Intel Mac 上的 AMD 独显，比固定显示“Apple GPU”更可靠。
        const GLubyte* rendererName = glGetString(GL_RENDERER);
        if (rendererName && rendererName[0]) {
            m_deviceName = reinterpret_cast<const char*>(rendererName);
        }
        LOG_INFO("VideoToolbox OpenGL renderer: " + m_deviceName);

        // 使用两个纹理槽轮换。某一槽被新 IOSurface 重绑前，先等待该槽上一次
        // OpenGL 采样完成；另一个槽可继续作为暂停画面，避免每帧 glFinish。
        for (auto& slot : m_slots) {
            glGenTextures(1, &slot.textureY);
            glGenTextures(1, &slot.textureUV);
            configureRectangleTexture(slot.textureY);
            configureRectangleTexture(slot.textureUV);
        }

        LOG_INFO("VideoToolbox zero-copy path enabled: CVPixelBuffer/IOSurface -> "
                 "CGL texture -> OpenGL shader");
        return true;
    }

    bool beginFrame(const AVFrame* frame, HardwareTextureBinding& binding) override {
        if (!frame || frame->format != AV_PIX_FMT_VIDEOTOOLBOX || !frame->data[3] ||
            CGLGetCurrentContext() != m_context) {
            return false;
        }

        auto pixelBuffer = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
        if (CVPixelBufferGetPlaneCount(pixelBuffer) != 2) {
            LOG_ERROR("VideoToolbox zero-copy path requires a two-plane NV12/P010 buffer");
            return false;
        }

        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixelBuffer);
        if (!surface) {
            // FFmpeg 的 VideoToolbox 会请求 IOSurface/OpenGL compatible pixel buffer。
            // 若驱动仍返回普通内存缓冲，则必须拒绝该帧，绝不能锁定 base address 回退。
            LOG_ERROR("VideoToolbox frame is not backed by IOSurface");
            return false;
        }

        const OSType pixelFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);
        const AVPixelFormat softwareFormat =
            av_map_videotoolbox_format_to_pixfmt(pixelFormat);
        GLenum yInternalFormat = GL_R8;
        GLenum uvInternalFormat = GL_RG8;
        GLenum componentType = GL_UNSIGNED_BYTE;

        bool supportedFormat = softwareFormat == AV_PIX_FMT_NV12;
        if (softwareFormat == AV_PIX_FMT_P010) {
            yInternalFormat = GL_R16;
            uvInternalFormat = GL_RG16;
            componentType = GL_UNSIGNED_SHORT;
            supportedFormat = true;
        }

        if (!supportedFormat) {
            LOG_ERROR("Unsupported VideoToolbox IOSurface pixel format: " +
                      std::to_string(static_cast<unsigned int>(pixelFormat)));
            return false;
        }

        const int nextSlot = m_activeSlot < 0 ? 0 : (m_activeSlot + 1) % 2;
        Slot& slot = m_slots[nextSlot];
        waitAndReleaseSlot(slot);

        AVFrame* retainedFrame = av_frame_alloc();
        if (!retainedFrame || av_frame_ref(retainedFrame, frame) < 0) {
            av_frame_free(&retainedFrame);
            LOG_ERROR("Failed to retain VideoToolbox frame for GPU rendering");
            return false;
        }

        const GLsizei yWidth =
            static_cast<GLsizei>(CVPixelBufferGetWidthOfPlane(pixelBuffer, 0));
        const GLsizei yHeight =
            static_cast<GLsizei>(CVPixelBufferGetHeightOfPlane(pixelBuffer, 0));
        const GLsizei uvWidth =
            static_cast<GLsizei>(CVPixelBufferGetWidthOfPlane(pixelBuffer, 1));
        const GLsizei uvHeight =
            static_cast<GLsizei>(CVPixelBufferGetHeightOfPlane(pixelBuffer, 1));

        glBindTexture(GL_TEXTURE_RECTANGLE, slot.textureY);
        CGLError error = CGLTexImageIOSurface2D(
            m_context,
            GL_TEXTURE_RECTANGLE,
            yInternalFormat,
            yWidth,
            yHeight,
            GL_RED,
            componentType,
            surface,
            0);
        if (error != kCGLNoError) {
            av_frame_free(&retainedFrame);
            glBindTexture(GL_TEXTURE_RECTANGLE, 0);
            LOG_ERROR("Failed to map IOSurface luma plane to OpenGL: " +
                      std::to_string(static_cast<int>(error)));
            return false;
        }

        glBindTexture(GL_TEXTURE_RECTANGLE, slot.textureUV);
        error = CGLTexImageIOSurface2D(
            m_context,
            GL_TEXTURE_RECTANGLE,
            uvInternalFormat,
            uvWidth,
            uvHeight,
            GL_RG,
            componentType,
            surface,
            1);
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        if (error != kCGLNoError) {
            av_frame_free(&retainedFrame);
            LOG_ERROR("Failed to map IOSurface chroma plane to OpenGL: " +
                      std::to_string(static_cast<int>(error)));
            return false;
        }

        // AVFrame 引用间接 retain CVPixelBuffer/IOSurface，直到该槽的 GPU fence
        // 完成后才释放，防止 VideoToolbox 过早复用仍在被 OpenGL 采样的 surface。
        slot.frame = retainedFrame;
        slot.width = yWidth;
        slot.height = yHeight;
        slot.uvWidth = uvWidth;
        slot.uvHeight = uvHeight;
        m_activeSlot = nextSlot;

        fillBinding(slot, binding);
        return true;
    }

    bool beginCachedFrame(HardwareTextureBinding& binding) override {
        if (m_activeSlot < 0 || !m_slots[m_activeSlot].frame) {
            return false;
        }
        fillBinding(m_slots[m_activeSlot], binding);
        return true;
    }

    void endFrame() override {
        if (m_activeSlot < 0) {
            return;
        }

        Slot& slot = m_slots[m_activeSlot];
        if (slot.fence) {
            // 同一槽的命令按 OpenGL 提交顺序执行，只需保留最后一个 fence；
            // 等待最后 fence 即可覆盖此前对该 IOSurface 的全部读取。
            glDeleteSync(slot.fence);
        }
        slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    }

private:
    struct Slot {
        GLuint textureY = 0;
        GLuint textureUV = 0;
        AVFrame* frame = nullptr;
        GLsync fence = nullptr;
        int width = 0;
        int height = 0;
        int uvWidth = 0;
        int uvHeight = 0;
    };

    static void configureRectangleTexture(GLuint texture) {
        glBindTexture(GL_TEXTURE_RECTANGLE, texture);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    }

    static void fillBinding(const Slot& slot, HardwareTextureBinding& binding) {
        binding.layout = HardwareTextureLayout::NV12Rectangle;
        binding.textureTarget = GL_TEXTURE_RECTANGLE;
        binding.textureRGBA = 0;
        binding.textureY = slot.textureY;
        binding.textureUV = slot.textureUV;
        binding.width = slot.width;
        binding.height = slot.height;
        binding.uvWidth = slot.uvWidth;
        binding.uvHeight = slot.uvHeight;
    }

    static void waitAndReleaseSlot(Slot& slot) {
        if (slot.fence) {
            GLenum waitResult = glClientWaitSync(
                slot.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
            if (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED) {
                // 一秒仍未完成通常意味着上下文/驱动异常。glFinish 是最后的 GPU
                // 同步保险，但仍不会把像素复制到 CPU。
                glFinish();
            }
            glDeleteSync(slot.fence);
            slot.fence = nullptr;
        }
        if (slot.frame) {
            av_frame_free(&slot.frame);
        }
        slot.width = 0;
        slot.height = 0;
        slot.uvWidth = 0;
        slot.uvHeight = 0;
    }

    void destroy() {
        for (auto& slot : m_slots) {
            waitAndReleaseSlot(slot);
            if (slot.textureY) {
                glDeleteTextures(1, &slot.textureY);
                slot.textureY = 0;
            }
            if (slot.textureUV) {
                glDeleteTextures(1, &slot.textureUV);
                slot.textureUV = 0;
            }
        }
        m_activeSlot = -1;
        m_context = nullptr;
    }

    CGLContextObj m_context = nullptr;
    Slot m_slots[2];
    int m_activeSlot = -1;
    std::string m_deviceName = "macOS GPU";
};

#else

class UnsupportedHardwareInterop final : public HardwareFrameInterop {
public:
    const char* backendName() const override {
        return "Software";
    }

    const char* deviceName() const override {
        return "CPU";
    }

    const char* zeroCopyMode() const override {
        return "Disabled";
    }

    bool initialize(AVHWDeviceType, AVBufferRef*, int, int) override {
        LOG_INFO("Zero-copy hardware/OpenGL interop is unavailable on this platform");
        return false;
    }

    bool beginFrame(const AVFrame*, HardwareTextureBinding&) override {
        return false;
    }

    bool beginCachedFrame(HardwareTextureBinding&) override {
        return false;
    }

    void endFrame() override {
    }
};

#endif

} // namespace

std::unique_ptr<HardwareFrameInterop> createHardwareFrameInterop() {
#if defined(_WIN32)
    return std::make_unique<D3D11OpenGLInterop>();
#elif defined(__APPLE__)
    return std::make_unique<VideoToolboxOpenGLInterop>();
#else
    return std::make_unique<UnsupportedHardwareInterop>();
#endif
}

} // namespace FluxPlayer
