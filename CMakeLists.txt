# CMake 最低版本要求。3.16 起支持预编译头（target_precompile_headers）
cmake_minimum_required(VERSION 3.16)

# 声明项目名称、版本，启用 C 和 C++ 两种语言
project(FluxPlayer VERSION 0.1.0 LANGUAGES C CXX)

# 要求使用 C++17 标准，且不允许降级
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json，供 clangd/ccache 等工具使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ========================
# 配置路径
# ========================
# 如果外部没有通过 -DFFMPEG_ROOT=... 指定路径，则按平台设置默认值
if(NOT DEFINED FFMPEG_ROOT)
    if(WIN32)
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
    elseif(APPLE)
        # macOS：使用项目内的预编译版本（与 Windows 对称）
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-macos")
    else()
        set(FFMPEG_ROOT "/usr/local")
    endif()
endif()

# ========================
# 1. GLAD（OpenGL 函数加载器）
# ========================
# add_library(名称 类型 源文件...) — 编译为静态库 .a/.lib
add_library(glad STATIC
    third_party/glad/src/glad.c
)
# PUBLIC 表示：使用 glad 的目标也会自动获得这个头文件搜索路径
target_include_directories(glad PUBLIC
    third_party/glad/include
)

# ========================
# 2. GLFW（窗口和 OpenGL 上下文管理）
# ========================
set(GLFW_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw-3.3.8)

# GLFW 各平台共用的源文件
set(GLFW_COMMON_SOURCES
    ${GLFW_SRC_DIR}/src/context.c
    ${GLFW_SRC_DIR}/src/init.c
    ${GLFW_SRC_DIR}/src/input.c
    ${GLFW_SRC_DIR}/src/monitor.c
    ${GLFW_SRC_DIR}/src/vulkan.c
    ${GLFW_SRC_DIR}/src/window.c
    ${GLFW_SRC_DIR}/src/egl_context.c
    ${GLFW_SRC_DIR}/src/osmesa_context.c
)

# 根据当前平台选择对应的平台源文件和宏定义
if(APPLE)
    # macOS 使用 Cocoa 后端，需要编译 Objective-C (.m) 文件
    enable_language(OBJC)
    set(GLFW_PLATFORM_SOURCES
        ${GLFW_SRC_DIR}/src/cocoa_init.m
        ${GLFW_SRC_DIR}/src/cocoa_joystick.m
        ${GLFW_SRC_DIR}/src/cocoa_monitor.m
        ${GLFW_SRC_DIR}/src/cocoa_window.m
        ${GLFW_SRC_DIR}/src/cocoa_time.c
        ${GLFW_SRC_DIR}/src/posix_thread.c
        ${GLFW_SRC_DIR}/src/nsgl_context.m
    )
    set(GLFW_PLATFORM_DEFINES _GLFW_COCOA)
elseif(WIN32)
    # Windows 使用 Win32 后端
    set(GLFW_PLATFORM_SOURCES
        ${GLFW_SRC_DIR}/src/win32_init.c
        ${GLFW_SRC_DIR}/src/win32_joystick.c
        ${GLFW_SRC_DIR}/src/win32_monitor.c
        ${GLFW_SRC_DIR}/src/win32_time.c
        ${GLFW_SRC_DIR}/src/win32_thread.c
        ${GLFW_SRC_DIR}/src/win32_window.c
        ${GLFW_SRC_DIR}/src/wgl_context.c
    )
    set(GLFW_PLATFORM_DEFINES _GLFW_WIN32)
elseif(UNIX)
    # Linux 使用 X11 后端
    set(GLFW_PLATFORM_SOURCES
        ${GLFW_SRC_DIR}/src/x11_init.c
        ${GLFW_SRC_DIR}/src/x11_monitor.c
        ${GLFW_SRC_DIR}/src/x11_window.c
        ${GLFW_SRC_DIR}/src/xkb_unicode.c
        ${GLFW_SRC_DIR}/src/posix_time.c
        ${GLFW_SRC_DIR}/src/posix_thread.c
        ${GLFW_SRC_DIR}/src/glx_context.c
        ${GLFW_SRC_DIR}/src/linux_joystick.c
    )
    set(GLFW_PLATFORM_DEFINES _GLFW_X11)
endif()

add_library(glfw STATIC
    ${GLFW_COMMON_SOURCES}
    ${GLFW_PLATFORM_SOURCES}
)
# PUBLIC：glfw 的头文件路径会传递给所有依赖 glfw 的目标
target_include_directories(glfw PUBLIC
    ${GLFW_SRC_DIR}/include
)
# PRIVATE：平台宏只在编译 glfw 自身时生效，不传递给使用者
target_compile_definitions(glfw PRIVATE ${GLFW_PLATFORM_DEFINES})

# macOS 的 .m 文件需要禁用 ARC（自动引用计数），GLFW 自己管理内存
if(APPLE)
    set_source_files_properties(
        ${GLFW_SRC_DIR}/src/cocoa_init.m
        ${GLFW_SRC_DIR}/src/cocoa_joystick.m
        ${GLFW_SRC_DIR}/src/cocoa_monitor.m
        ${GLFW_SRC_DIR}/src/cocoa_window.m
        ${GLFW_SRC_DIR}/src/nsgl_context.m
        PROPERTIES COMPILE_FLAGS "-fno-objc-arc"
    )
endif()

# 禁用 GLFW 的编译警告（第三方库，不需要关注其警告）
# $<...> 是生成器表达式，意思是：非 MSVC 编译器才加 -w 参数
target_compile_options(glfw PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

# ========================
# 3. ImGui（即时模式 GUI 库）
# ========================
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp       # 演示窗口，可在 debug 时查看所有控件
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp     # GLFW 输入后端
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp  # OpenGL3 渲染后端
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
# imgui 依赖 glfw 和 glad，PUBLIC 表示使用 imgui 的目标也会自动链接它们
target_link_libraries(imgui PUBLIC glfw glad)
target_compile_options(imgui PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>)

# ========================
# 4. tinyfiledialogs（系统文件对话框）
# ========================
# 纯 C 文件，单独编译为静态库，避免与 C++ 预编译头（PCH）冲突
add_library(tinyfiledialogs STATIC third_party/tinyfiledialogs/tinyfiledialogs.c)
target_include_directories(tinyfiledialogs PUBLIC third_party/tinyfiledialogs)

# ========================
# 5. FluxPlayer 主程序
# ========================
set(FLUXPLAYER_SOURCES
    src/main.cpp
    src/audio/AudioOutput.cpp
    src/core/AVSync.cpp
    src/core/FrameQueue.cpp
    src/core/MediaInfo.cpp
    src/core/Player.cpp
    src/decoder/AudioDecoder.cpp
    src/decoder/Demuxer.cpp
    src/decoder/Frame.cpp
    src/decoder/VideoDecoder.cpp
    src/renderer/GLRenderer.cpp
    src/renderer/Shader.cpp
    src/subtitle/SubtitleDecoder.cpp
    src/subtitle/SubtitleManager.cpp
    src/ui/Controller.cpp
    src/ui/HomeScreen.cpp
    src/ui/Window.cpp
    src/utils/Config.cpp
    src/utils/Logger.cpp
    src/utils/Screenshot.cpp
    src/utils/Timer.cpp
    src/recorder/Recorder.cpp
    src/video/FrameInterpolator.cpp
)

# add_executable：将源文件编译并链接为可执行程序
add_executable(FluxPlayer ${FLUXPLAYER_SOURCES})

# 预编译头（PCH）：把常用的 STL 头和 Logger.h 预先编译好，
# 后续每个 .cpp 直接复用，避免重复解析，显著加快编译速度
target_precompile_headers(FluxPlayer PRIVATE "include/FluxPlayer/pch.h")

# 项目自身的头文件搜索路径（PRIVATE：只对 FluxPlayer 自身生效）
target_include_directories(FluxPlayer PRIVATE
    include           # 项目头文件根目录
    third_party/glm   # 数学库（仅头文件）
)

# 链接第三方静态库
target_link_libraries(FluxPlayer PRIVATE glfw glad imgui tinyfiledialogs)

# FFmpeg：添加头文件路径和库搜索路径，然后链接各个模块
target_include_directories(FluxPlayer PRIVATE ${FFMPEG_ROOT}/include)
target_link_directories(FluxPlayer PRIVATE ${FFMPEG_ROOT}/lib)
target_link_libraries(FluxPlayer PRIVATE avformat avcodec avutil swscale swresample)

# 平台相关的系统库链接
if(APPLE)
    # macOS 使用 framework 形式的系统库
    find_library(OPENGL_FRAMEWORK OpenGL)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)  # 音频输出
    target_link_libraries(FluxPlayer PRIVATE
        ${OPENGL_FRAMEWORK}
        ${COCOA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
    )
elseif(WIN32)
    target_link_libraries(FluxPlayer PRIVATE
        opengl32    # OpenGL
        gdi32       # Windows 图形设备接口
        winmm       # Windows 多媒体（音频输出）
        ole32       # COM 组件
        comdlg32    # 文件对话框
        d3d11       # D3D11VA 硬件解码
        dxgi        # DXGI 设备枚举
    )
elseif(UNIX)
    target_link_libraries(FluxPlayer PRIVATE
        GL          # OpenGL
        X11         # X 窗口系统
        pthread     # 多线程
        dl          # 动态库加载
        asound      # ALSA 音频输出
    )
endif()

# Debug 模式下定义 DEBUG 宏，用于条件编译调试代码
# $<$<CONFIG:Debug>:DEBUG> 是生成器表达式，支持 Xcode/VS 等多配置生成器
target_compile_definitions(FluxPlayer PRIVATE $<$<CONFIG:Debug>:DEBUG>)

# TCP 日志选项：编译时可通过 -DENABLE_TCP_LOG=ON 开启，
# 开启后可用 nc <ip> <port> 远程查看实时日志
option(ENABLE_TCP_LOG "Enable TCP log server for remote log viewing" OFF)
if(ENABLE_TCP_LOG)
    target_compile_definitions(FluxPlayer PRIVATE ENABLE_TCP_LOG)
endif()

# macOS：设置 rpath，让可执行文件在构建目录和安装后都能找到 dylib
if(APPLE)
    set_target_properties(FluxPlayer PROPERTIES
        BUILD_RPATH "@executable_path"
        INSTALL_RPATH "@executable_path"
    )
endif()

# 构建完成后，自动把 assets/shaders/ 目录复制到可执行文件旁边
# $<TARGET_FILE_DIR:FluxPlayer> 会自动解析为实际的输出目录
add_custom_command(TARGET FluxPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
        $<TARGET_FILE_DIR:FluxPlayer>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts
        $<TARGET_FILE_DIR:FluxPlayer>/fonts
    COMMENT "Copying shader files..."
)

# Windows：构建完成后把 FFmpeg 的 DLL 复制到可执行文件旁边，否则运行时找不到
if(WIN32)
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    if(FFMPEG_DLLS)
        add_custom_command(TARGET FluxPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_DLLS}
                $<TARGET_FILE_DIR:FluxPlayer>
            COMMENT "Copying FFmpeg DLLs..."
        )
    endif()
endif()

# macOS：构建完成后把 FFmpeg 的 dylib 复制到可执行文件旁边（与 Windows 对称）
if(APPLE)
    file(GLOB FFMPEG_DYLIBS "${FFMPEG_ROOT}/lib/*.dylib")
    if(FFMPEG_DYLIBS)
        add_custom_command(TARGET FluxPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_DYLIBS}
                $<TARGET_FILE_DIR:FluxPlayer>
            COMMENT "Copying FFmpeg dylibs..."
        )
    endif()
endif()
