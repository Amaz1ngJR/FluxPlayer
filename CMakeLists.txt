cmake_minimum_required(VERSION 3.15)
project(FluxPlayer VERSION 0.1.0 LANGUAGES CXX C)

# ========================
# 项目配置
# ========================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ========================
# 构建类型配置
# ========================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DDEBUG)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
endif()

# ========================
# 平台检测和配置
# ========================
if(APPLE)
    set(PLATFORM_MACOS TRUE)
    message(STATUS "Building for macOS")
elseif(WIN32)
    set(PLATFORM_WINDOWS TRUE)
    message(STATUS "Building for Windows")
elseif(UNIX)
    set(PLATFORM_LINUX TRUE)
    message(STATUS "Building for Linux")
endif()

# ========================
# FFmpeg 配置
# ========================
if(APPLE)
    # macOS 使用 Homebrew 安装的 FFmpeg 4
    set(FFMPEG_ROOT "/opt/homebrew/opt/ffmpeg@4" CACHE PATH "FFmpeg installation path")
    if(NOT EXISTS ${FFMPEG_ROOT})
        # 尝试 Intel Mac 的路径
        set(FFMPEG_ROOT "/usr/local/opt/ffmpeg@4")
    endif()
else()
    # Linux/Windows 需要设置 FFMPEG_ROOT
    set(FFMPEG_ROOT "" CACHE PATH "FFmpeg installation path")
endif()

if(EXISTS ${FFMPEG_ROOT})
    include_directories(${FFMPEG_ROOT}/include)
    link_directories(${FFMPEG_ROOT}/lib)
    message(STATUS "FFmpeg found at: ${FFMPEG_ROOT}")
else()
    message(WARNING "FFmpeg not found at: ${FFMPEG_ROOT}")
    message(WARNING "Please set FFMPEG_ROOT to your FFmpeg installation path")
endif()

# ========================
# 1. GLFW 库（本地第三方库）
# ========================
set(GLFW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glfw-3.3.8)
file(GLOB GLFW_SOURCES
    ${GLFW_DIR}/src/context.c
    ${GLFW_DIR}/src/init.c
    ${GLFW_DIR}/src/input.c
    ${GLFW_DIR}/src/monitor.c
    ${GLFW_DIR}/src/vulkan.c
    ${GLFW_DIR}/src/window.c
)

if(PLATFORM_MACOS)
    file(GLOB GLFW_PLATFORM_SOURCES
        ${GLFW_DIR}/src/cocoa_*.m
        ${GLFW_DIR}/src/nsgl_*.m
        ${GLFW_DIR}/src/posix_thread.c
        ${GLFW_DIR}/src/cocoa_*.c
        ${GLFW_DIR}/src/nsgl_*.c
    )
    # 添加上下文支持文件
    list(APPEND GLFW_PLATFORM_SOURCES
        ${GLFW_DIR}/src/egl_context.c
        ${GLFW_DIR}/src/osmesa_context.c
    )
    set(GLFW_DEFINITIONS _GLFW_COCOA)

    # 查找 macOS 框架
    find_library(COCOA_LIB Cocoa REQUIRED)
    find_library(COREVIDEO_LIB CoreVideo REQUIRED)
    find_library(IOKIT_LIB IOKit REQUIRED)
    set(GLFW_FRAMEWORKS ${COCOA_LIB} ${COREVIDEO_LIB} ${IOKIT_LIB})
elseif(PLATFORM_WINDOWS)
    file(GLOB GLFW_PLATFORM_SOURCES
        ${GLFW_DIR}/src/win32_*.c
        ${GLFW_DIR}/src/wgl_*.c
        ${GLFW_DIR}/src/posix_time.c
        ${GLFW_DIR}/src/posix_thread.c
        ${GLFW_DIR}/src/egl_context.c
        ${GLFW_DIR}/src/osmesa_context.c
    )
    set(GLFW_DEFINITIONS _GLFW_WIN32)
    set(GLFW_LIBRARIES gdi32 shell32)
elseif(PLATFORM_LINUX)
    file(GLOB GLFW_PLATFORM_SOURCES
        ${GLFW_DIR}/src/x11_*.c
        ${GLFW_DIR}/src/xkb_*.c
        ${GLFW_DIR}/src/glx_*.c
        ${GLFW_DIR}/src/posix_*.c
        ${GLFW_DIR}/src/linux_*.c
        ${GLFW_DIR}/src/egl_context.c
        ${GLFW_DIR}/src/osmesa_context.c
    )
    set(GLFW_DEFINITIONS _GLFW_X11)
    set(GLFW_LIBRARIES X11 Xrandr Xinerama Xcursor pthread dl)
endif()

add_library(glfw_local STATIC ${GLFW_SOURCES} ${GLFW_PLATFORM_SOURCES})
target_include_directories(glfw_local PUBLIC ${GLFW_DIR}/include)
target_compile_definitions(glfw_local PRIVATE ${GLFW_DEFINITIONS})

if(PLATFORM_MACOS)
    set_source_files_properties(
        ${GLFW_PLATFORM_SOURCES}
        PROPERTIES COMPILE_FLAGS "-fno-objc-arc"
    )
    target_link_libraries(glfw_local PUBLIC ${GLFW_FRAMEWORKS})
else()
    target_link_libraries(glfw_local PUBLIC ${GLFW_LIBRARIES})
endif()

# 禁用 GLFW 的警告
target_compile_options(glfw_local PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
    $<$<CXX_COMPILER_ID:MSVC>:/w>
)

# ========================
# 2. GLAD 库（本地第三方库）
# ========================
set(GLAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad)
add_library(glad_local STATIC ${GLAD_DIR}/src/glad.c)
target_include_directories(glad_local PUBLIC ${GLAD_DIR}/include)

# ========================
# 3. ImGui 库（本地第三方库）
# ========================
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
file(GLOB IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_library(imgui_local STATIC ${IMGUI_SOURCES})
target_include_directories(imgui_local PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui_local PUBLIC glfw_local glad_local)

# 禁用 ImGui 的警告
target_compile_options(imgui_local PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
    $<$<CXX_COMPILER_ID:MSVC>:/w>
)

# ========================
# 4. GLM 库（仅头文件）
# ========================
set(GLM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm)

# ========================
# 5. 主程序目标
# ========================
# 收集所有源文件
file(GLOB_RECURSE FLUXPLAYER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# 创建可执行文件
add_executable(FluxPlayer ${FLUXPLAYER_SOURCES})

# 包含头文件目录
target_include_directories(FluxPlayer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GLFW_DIR}/include
    ${GLAD_DIR}/include
    ${GLM_DIR}
)

# 链接第三方库
target_link_libraries(FluxPlayer PRIVATE
    glfw_local
    glad_local
    imgui_local
)

# 链接 FFmpeg 库
target_link_libraries(FluxPlayer PRIVATE
    avformat
    avcodec
    avutil
    swscale
    swresample
)

# 平台相关链接
if(PLATFORM_MACOS)
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
elseif(PLATFORM_WINDOWS)
    target_link_libraries(FluxPlayer PRIVATE
        opengl32
        gdi32
        winmm
    )
elseif(PLATFORM_LINUX)
    target_link_libraries(FluxPlayer PRIVATE
        GL
        X11
        pthread
        dl
        asound
    )
endif()

# ========================
# 复制着色器文件到构建目录
# ========================
add_custom_command(TARGET FluxPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
        $<TARGET_FILE_DIR:FluxPlayer>/shaders
    COMMENT "Copying shader files to build directory"
)

# ========================
# 安装配置
# ========================
install(TARGETS FluxPlayer
    RUNTIME DESTINATION bin
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
    DESTINATION bin/shaders
)

# ========================
# 打印构建信息
# ========================
message(STATUS "")
message(STATUS "==========================================")
message(STATUS "🎬 FluxPlayer - FFmpeg + OpenGL 跨平台播放器")
message(STATUS "📦 Build Configuration:")
message(STATUS "   Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "   C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "   Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "   FFmpeg Root: ${FFMPEG_ROOT}")
message(STATUS "==========================================")
message(STATUS "💡 Build commands:")
message(STATUS "   mkdir build && cd build")
message(STATUS "   cmake ..")
message(STATUS "   cmake --build .")
message(STATUS "   ./FluxPlayer <video_file>")
message(STATUS "==========================================")
message(STATUS "")
