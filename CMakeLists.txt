# CMake 最低版本要求。3.16 起支持预编译头（target_precompile_headers）
cmake_minimum_required(VERSION 3.16)

# 声明项目名称、版本，启用 C 和 C++ 两种语言
project(FluxPlayer VERSION 0.8.3 LANGUAGES C CXX)

# macOS 上需要 Objective-C++ 才能编译 WebLogin_mac.mm（WKWebView 桥接层）
if(APPLE)
    enable_language(OBJCXX)
endif()

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
# CONFIGURE_DEPENDS：新增/删除 .cpp 时 cmake --build 自动重新 glob，无需手动 cmake
file(GLOB_RECURSE FLUXPLAYER_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# 平台相关 WebLogin：glob 会抓入两个实现，先移除非本平台的，再追加 .mm
if(APPLE)
    list(REMOVE_ITEM FLUXPLAYER_SOURCES "${CMAKE_SOURCE_DIR}/src/utils/WebLogin_win.cpp")
    list(APPEND FLUXPLAYER_SOURCES src/utils/WebLogin_mac.mm)
    set_source_files_properties(src/utils/WebLogin_mac.mm
        PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
elseif(WIN32)
    # WebLogin_win.cpp 已被 glob 收录，WebLogin_mac.mm 是 .mm 不在 glob 里，无需处理
endif()

# add_executable：将源文件编译并链接为可执行程序
if(WIN32)
    # source/app.rc 把 source/pic.ico 嵌入 exe，让资源管理器/任务栏/快捷方式都显示图标
    # （缺此步快捷方式图标为空白）。windres 默认在 .rc 所在目录搜索 pic.ico。
    add_executable(FluxPlayer WIN32 ${FLUXPLAYER_SOURCES} source/app.rc)
else()
    add_executable(FluxPlayer ${FLUXPLAYER_SOURCES})
endif()

# CMake 的缓存、对象文件和静态库保留在 -B 指定的 build/cmake 中；
# 只有可直接运行的主程序进入共享的 build/bin。xmake 也把主程序及运行时依赖
# 输出到同一目录，因此切换构建系统后可以直接运行同一条路径。
#
# 多配置生成器（Visual Studio/Xcode）默认会追加 Debug/Release 子目录。
# 为每种标准配置显式设置输出路径，保证所有生成器都遵守 build/bin 约定。
set(FLUXPLAYER_RUNTIME_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/bin")
set_target_properties(FluxPlayer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FLUXPLAYER_RUNTIME_OUTPUT_DIR}"
)
foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(FluxPlayer PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_config}" "${FLUXPLAYER_RUNTIME_OUTPUT_DIR}"
    )
endforeach()

# yt-dlp 打包路径：编译时固定，避免运行时路径推算失败
if(APPLE)
    set(YTDLP_BUNDLED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/yt-dlp/yt-dlp_macos")
elseif(WIN32)
    set(YTDLP_BUNDLED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/yt-dlp/yt-dlp.exe")
else()
    set(YTDLP_BUNDLED_PATH "")
endif()
string(REPLACE "\\" "/" YTDLP_BUNDLED_PATH "${YTDLP_BUNDLED_PATH}")
if(EXISTS "${YTDLP_BUNDLED_PATH}")
    target_compile_definitions(FluxPlayer PRIVATE
        YTDLP_BUNDLED_PATH="${YTDLP_BUNDLED_PATH}")
endif()

# 传递版本号宏到代码（从 project VERSION 读取，用于 UI 显示和打包）
target_compile_definitions(FluxPlayer PRIVATE
    FLUXPLAYER_VERSION="${CMAKE_PROJECT_VERSION}"
)

# 预编译头（PCH）：把常用的 STL 头和 Logger.h 预先编译好，
# 后续每个 .cpp 直接复用，避免重复解析，显著加快编译速度
target_precompile_headers(FluxPlayer PRIVATE "include/FluxPlayer/pch.h")

# 项目自身的头文件搜索路径（PRIVATE：只对 FluxPlayer 自身生效）
target_include_directories(FluxPlayer PRIVATE
    include              # 项目头文件根目录
    third_party/glm      # 数学库（仅头文件）
    third_party          # nlohmann/json 等仅头文件库的根目录（include 形式 <nlohmann/json.hpp>）
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
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox)
    find_library(COREMEDIA_FRAMEWORK CoreMedia)
    find_library(IOSURFACE_FRAMEWORK IOSurface)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)  # 音频输出
    find_library(WEBKIT_FRAMEWORK WebKit)              # WKWebView 内置登录窗口
    target_link_libraries(FluxPlayer PRIVATE
        ${OPENGL_FRAMEWORK}
        ${COCOA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${VIDEOTOOLBOX_FRAMEWORK}
        ${COREMEDIA_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
        ${WEBKIT_FRAMEWORK}
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
        shell32     # SHGetFolderPath（应用数据目录）
        uuid        # IID_IUnknown 等 COM IID（与 xmake Windows 链接项一致）
    )

    # WebView2 SDK 检测：放在 third_party/webview2/ 下时自动启用内置登录窗口
    # 期望文件：
    #   - third_party/webview2/include/WebView2.h
    #     （微软官方单头文件，MIT/Apache-2.0）
    #   - third_party/webview2/runtimes/win-x64/WebView2Loader.dll
    #     （来自 Microsoft.Web.WebView2 NuGet 包；不需要 .lib，运行时通过
    #      LoadLibraryW 动态加载，但该 DLL 不随 WebView2 Runtime 分发，
    #      必须由应用自带——构建时拷到 exe 旁边）
    set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
    if(EXISTS "${WEBVIEW2_ROOT}/include/WebView2.h")
        message(STATUS "WebView2 SDK header detected: ${WEBVIEW2_ROOT}/include/WebView2.h")
        target_compile_definitions(FluxPlayer PRIVATE FLUXPLAYER_HAVE_WEBVIEW2)
        target_include_directories(FluxPlayer PRIVATE "${WEBVIEW2_ROOT}/include")

        set(WEBVIEW2_LOADER_DLL "${WEBVIEW2_ROOT}/runtimes/win-x64/WebView2Loader.dll")
        if(EXISTS "${WEBVIEW2_LOADER_DLL}")
            message(STATUS "WebView2Loader.dll detected: ${WEBVIEW2_LOADER_DLL}")
            add_custom_command(TARGET FluxPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WEBVIEW2_LOADER_DLL}"
                    $<TARGET_FILE_DIR:FluxPlayer>
                COMMENT "Copying WebView2Loader.dll"
            )
            # WebView2Loader.dll 是运行时 LoadLibraryW 动态加载的（不在 exe 的
            # PE 导入表里），file(GET_RUNTIME_DEPENDENCIES) 扫不到它，因此必须显式
            # 安装到 staging，否则 Inno Setup 打包后安装的程序找不到该 DLL。
            install(FILES "${WEBVIEW2_LOADER_DLL}" DESTINATION .)
        else()
            message(WARNING "WebView2Loader.dll 未找到: ${WEBVIEW2_LOADER_DLL}\n"
                            "请从 Microsoft.Web.WebView2 NuGet 包提取 runtimes/win-x64/native/WebView2Loader.dll，\n"
                            "放到 third_party/webview2/runtimes/win-x64/ 下后重新构建；\n"
                            "否则运行时会报『加载 WebView2Loader.dll 失败』。")
        endif()
    else()
        message(STATUS "WebView2 SDK header 未检测到（third_party/webview2/include/WebView2.h 缺失），"
                       "内置登录窗口将编译为占位实现，运行时返回 Unsupported")
    endif()
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
    # source/ 全拷贝（开发期依赖，含字幕测试、封面兜底图）
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/source
        $<TARGET_FILE_DIR:FluxPlayer>/source
    # 皮肤包内置回退路径：与开发期 source/UI/skins 解耦，
    # 让发布版即便缺失开发目录也能从 resources/skins 加载到内置 cyberpunk-neon
    # 只拷贝运行时必需文件（skin.json + preview.svg），排除设计稿 mockup_*.svg
    # 先清空目标目录：避免旧构建（曾整目录拷贝）残留的 mockup 滞留
    COMMAND ${CMAKE_COMMAND} -E remove_directory
        $<TARGET_FILE_DIR:FluxPlayer>/resources/skins/cyberpunk-neon
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:FluxPlayer>/resources/skins/cyberpunk-neon
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/source/UI/skins/cyberpunk-neon/skin.json
        $<TARGET_FILE_DIR:FluxPlayer>/resources/skins/cyberpunk-neon/skin.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/source/UI/skins/cyberpunk-neon/preview.svg
        $<TARGET_FILE_DIR:FluxPlayer>/resources/skins/cyberpunk-neon/preview.svg
    COMMENT "Copying runtime resources..."
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

# Windows + MinGW：把 3 个 GCC 运行时 DLL 拷到 exe 旁边，方便直接从 build/bin 启动调试
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_filename_component(_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${_MINGW_BIN_DIR}/${_dll}")
            add_custom_command(TARGET FluxPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_MINGW_BIN_DIR}/${_dll}"
                    $<TARGET_FILE_DIR:FluxPlayer>
            )
        endif()
    endforeach()
endif()

# ========================
# 6. 安装规则（用于打包）
# ========================
# 用法：cmake --install <build> --prefix <staging>
#   把 exe、shaders、fonts 装到 <staging>，再用 file(GET_RUNTIME_DEPENDENCIES)
#   按 PE 导入表递归解析 DLL 依赖闭包（FFmpeg + 其 codec 子依赖 + MinGW 运行时），
#   排除 Windows 系统 DLL。两台机器装出来的内容一致，Inno Setup 只需抓 staging。
install(TARGETS FluxPlayer RUNTIME DESTINATION .)
install(DIRECTORY assets/shaders/ DESTINATION shaders)
install(DIRECTORY assets/fonts/   DESTINATION fonts)
# source/ 安装策略：仅安装运行时必需文件，不安装整个 UI/ 设计目录。
# pic2.png 是纯音频封面兜底图，Player 通过 Config::getResourcePath("pic2.png")
# 加载（Win/Linux 解析为 <exe>/resources/），故装到 resources/ 而非 source/
install(FILES source/pic2.png DESTINATION resources)
# video_01.mp4 是启动真实解码测速样片，HardwareInfo 通过
# Config::getResourcePath("video/video_01.mp4") 查找。
install(FILES source/video/video_01.mp4 DESTINATION resources/video)
# 安装阶段同步把皮肤包当作只读资源放到 resources/skins，
# 与运行期 Config::getResourcePath("skins") 的解析路径对齐
# 只安装运行时必需文件（skin.json + preview.svg），排除设计稿 mockup_*.svg
install(FILES
    source/UI/skins/cyberpunk-neon/skin.json
    source/UI/skins/cyberpunk-neon/preview.svg
    DESTINATION resources/skins/cyberpunk-neon)

# yt-dlp 可执行文件：发布版必须随程序分发到 exe 同级，
# 否则安装到目标机后 StreamExtractor 找不到 yt-dlp（运行时报「找不到 dlp」）。
# 运行期由 getYtDlpPath() 优先在 exe 同级目录查找。
if(WIN32)
    install(PROGRAMS third_party/yt-dlp/yt-dlp.exe DESTINATION .)
elseif(APPLE)
    install(PROGRAMS third_party/yt-dlp/yt-dlp_macos DESTINATION .)
endif()

if(WIN32)
    get_filename_component(_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(CODE "
        # CMake 4.4+ 在分析 Windows PE 依赖前会规范化路径分隔符。
        # 显式启用新策略，避免为每个 System32 DLL 重复输出 CMP0207 警告；
        # 下面的排除正则不依赖斜杠方向，规范化不会改变依赖筛选结果。
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW)
        endif()
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES \"\${CMAKE_INSTALL_PREFIX}/FluxPlayer.exe\"
            RESOLVED_DEPENDENCIES_VAR   _r_deps
            UNRESOLVED_DEPENDENCIES_VAR _u_deps
            DIRECTORIES
                \"${FFMPEG_ROOT}/bin\"
                \"${_MINGW_BIN_DIR}\"
            PRE_EXCLUDE_REGEXES
                \"api-ms-.*\"
                \"ext-ms-.*\"
            POST_EXCLUDE_REGEXES
                \".*[Ss]ystem32.*\"
                \".*SysWOW64.*\"
        )
        foreach(_dep \${_r_deps})
            get_filename_component(_dep_name \"\${_dep}\" NAME)
            set(_dep_dst \"\${CMAKE_INSTALL_PREFIX}/\${_dep_name}\")
            # 找原始源文件（跳过 build/ 中转，避免 POST_BUILD copy 刷新时间戳导致误判）
            set(_dep_src \"\${_dep}\")
            foreach(_src_dir \"${FFMPEG_ROOT}/bin\" \"${_MINGW_BIN_DIR}\")
                if(EXISTS \"\${_src_dir}/\${_dep_name}\")
                    set(_dep_src \"\${_src_dir}/\${_dep_name}\")
                    break()
                endif()
            endforeach()
            if(NOT EXISTS \"\${_dep_dst}\" OR \"\${_dep_src}\" IS_NEWER_THAN \"\${_dep_dst}\")
                file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}\" TYPE SHARED_LIBRARY FILES \"\${_dep}\")
            endif()
        endforeach()
        foreach(_dep \${_u_deps})
            message(STATUS \"[install] Skipped unresolved: \${_dep}\")
        endforeach()
    ")
endif()
