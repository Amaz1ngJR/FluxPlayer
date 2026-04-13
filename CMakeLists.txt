cmake_minimum_required(VERSION 3.16)
project(FluxPlayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ========================
# 配置路径
# ========================
# FFmpeg 路径
if(NOT DEFINED FFMPEG_ROOT)
    if(WIN32)
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
    elseif(APPLE)
        set(FFMPEG_ROOT "/opt/homebrew/opt/ffmpeg@4")
    else()
        set(FFMPEG_ROOT "/usr/local")
    endif()
endif()

# ========================
# 1. GLAD 库（静态库）
# ========================
add_library(glad STATIC
    third_party/glad/src/glad.c
)
target_include_directories(glad PUBLIC
    third_party/glad/include
)

# ========================
# 2. GLFW 库（静态库，从源码编译）
# ========================
set(GLFW_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw-3.3.8)

# GLFW 公共源文件
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

if(APPLE)
    # macOS: Cocoa 后端
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
    # Windows
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
    # Linux: X11 后端
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
target_include_directories(glfw PUBLIC
    ${GLFW_SRC_DIR}/include
)
target_compile_definitions(glfw PRIVATE ${GLFW_PLATFORM_DEFINES})

# macOS: Objective-C 文件禁用 ARC
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

# 禁用 GLFW 编译警告
target_compile_options(glfw PRIVATE -w)

# ========================
# 3. ImGui 库（静态库）
# ========================
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw glad)
target_compile_options(imgui PRIVATE -w)

# ========================
# 4. 主程序目标
# ========================
set(FLUXPLAYER_SOURCES
    src/main.cpp
    src/audio/AudioOutput.cpp
    src/core/AVSync.cpp
    src/core/MediaInfo.cpp
    src/core/Player.cpp
    src/decoder/AudioDecoder.cpp
    src/decoder/Demuxer.cpp
    src/decoder/Frame.cpp
    src/decoder/VideoDecoder.cpp
    src/renderer/GLRenderer.cpp
    src/renderer/Shader.cpp
    src/ui/Controller.cpp
    src/ui/HomeScreen.cpp
    src/ui/Window.cpp
    src/utils/Config.cpp
    src/utils/Logger.cpp
    src/utils/Screenshot.cpp
    src/utils/Timer.cpp
    third_party/tinyfiledialogs/tinyfiledialogs.c
)

add_executable(FluxPlayer ${FLUXPLAYER_SOURCES})

# 项目头文件
target_include_directories(FluxPlayer PRIVATE
    include
    third_party/glm
    third_party/tinyfiledialogs
)

# 链接本地第三方库
target_link_libraries(FluxPlayer PRIVATE glfw glad imgui)

# FFmpeg
target_include_directories(FluxPlayer PRIVATE ${FFMPEG_ROOT}/include)
target_link_directories(FluxPlayer PRIVATE ${FFMPEG_ROOT}/lib)
target_link_libraries(FluxPlayer PRIVATE avformat avcodec avutil swscale swresample)

# 平台相关链接
if(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
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
        opengl32
        gdi32
        winmm
        ole32
        comdlg32
    )
elseif(UNIX)
    target_link_libraries(FluxPlayer PRIVATE
        GL
        X11
        pthread
        dl
        asound
    )
endif()

# 构建类型配置
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(FluxPlayer PRIVATE DEBUG)
endif()

# TCP日志系统（用 nc ip port 远程查看实时日志）
option(ENABLE_TCP_LOG "Enable TCP log server for remote log viewing" OFF)
if(ENABLE_TCP_LOG)
    target_compile_definitions(FluxPlayer PRIVATE ENABLE_TCP_LOG)
endif()

# 构建后复制着色器文件到可执行文件目录
add_custom_command(TARGET FluxPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
        $<TARGET_FILE_DIR:FluxPlayer>/shaders
    COMMENT "Copying shader files..."
)

# Windows: 构建后复制 FFmpeg DLL 到可执行文件目录
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
