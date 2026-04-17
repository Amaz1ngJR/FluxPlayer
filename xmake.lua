-- xmake.lua — FluxPlayer 构建配置
-- xmake 是一个基于 Lua 的跨平台构建工具，语法比 CMake 更简洁

-- 启用 debug/release 两种构建模式（通过 xmake f -m debug/release 切换）
add_rules("mode.debug", "mode.release")

-- 项目基本信息
set_project("FluxPlayer")
set_version("0.1.0")
set_languages("cxx17")  -- 要求 C++17 标准

-- ========================
-- 可选编译选项
-- ========================
-- 定义一个名为 tcp_log 的开关，默认关闭
-- 开启方式：xmake f --tcp_log=y
-- 开启后可用 nc <ip> <port> 远程查看实时日志
option("tcp_log")
    set_default(false)
    set_showmenu(true)
    set_description("Enable TCP log server for remote log viewing")
option_end()

-- ========================
-- FFmpeg 路径配置
-- ========================
-- 根据当前编译平台自动选择 FFmpeg 的安装路径
local ffmpeg_root = ""
if is_plat("windows") then
    ffmpeg_root = "third_party/ffmpeg"          -- Windows：使用项目内的预编译版本
elseif is_plat("macosx") then
    ffmpeg_root = "/opt/homebrew/opt/ffmpeg@4"  -- macOS：Homebrew 安装的 FFmpeg 4.x
else
    ffmpeg_root = "/usr/local"                  -- Linux：系统默认安装路径
end

-- ========================
-- 1. GLFW（窗口和 OpenGL 上下文管理）
-- ========================
target("glfw_local")
    set_kind("static")          -- 编译为静态库 .a/.lib
    set_group("third_party")    -- IDE 中归类到 third_party 分组
    set_warnings("none")        -- 禁用第三方库的编译警告

    -- {public = true} 表示头文件路径会传递给所有依赖此库的目标
    add_includedirs("third_party/glfw-3.3.8/include", {public = true})

    if is_plat("macosx") then
        -- macOS 使用 Cocoa 后端，需要编译 .m（Objective-C）文件
        -- {mflags = "-fno-objc-arc"} 禁用 ARC，GLFW 自己管理内存
        add_files("third_party/glfw-3.3.8/src/*.m", {mflags = "-fno-objc-arc"})
        add_files("third_party/glfw-3.3.8/src/*.c")

        -- 排除其他平台的源文件（macOS 用通配符添加了所有 .c，需要手动排除）
        remove_files("third_party/glfw-3.3.8/src/win32_*.c")
        remove_files("third_party/glfw-3.3.8/src/wgl_*.c")
        remove_files("third_party/glfw-3.3.8/src/wl_*.c")
        remove_files("third_party/glfw-3.3.8/src/x11_*.c")
        remove_files("third_party/glfw-3.3.8/src/posix_time.c")
        remove_files("third_party/glfw-3.3.8/src/null_*.c")
        remove_files("third_party/glfw-3.3.8/src/linux_*.c")
        remove_files("third_party/glfw-3.3.8/src/xkb_*.c")
        remove_files("third_party/glfw-3.3.8/src/glx_*.c")

        add_mxflags("-fno-objc-arc")  -- 对所有 .m 文件禁用 ARC
        add_frameworks("Cocoa", "CoreVideo", "IOKit")
        add_defines("_GLFW_COCOA")    -- 告诉 GLFW 使用 Cocoa 后端

    elseif is_plat("windows") then
        -- Windows 使用 Win32 后端，显式列出所需文件
        add_files("third_party/glfw-3.3.8/src/win32_*.c")
        add_files(
            "third_party/glfw-3.3.8/src/context.c",
            "third_party/glfw-3.3.8/src/init.c",
            "third_party/glfw-3.3.8/src/input.c",
            "third_party/glfw-3.3.8/src/monitor.c",
            "third_party/glfw-3.3.8/src/vulkan.c",
            "third_party/glfw-3.3.8/src/window.c",
            "third_party/glfw-3.3.8/src/egl_context.c",
            "third_party/glfw-3.3.8/src/osmesa_context.c",
            "third_party/glfw-3.3.8/src/wgl_context.c"
        )
        add_defines("_GLFW_WIN32")
        add_syslinks("gdi32", "shell32")

    elseif is_plat("linux") then
        -- Linux 使用 X11 后端
        add_files("third_party/glfw-3.3.8/src/x11_*.c")
        add_files(
            "third_party/glfw-3.3.8/src/context.c",
            "third_party/glfw-3.3.8/src/init.c",
            "third_party/glfw-3.3.8/src/input.c",
            "third_party/glfw-3.3.8/src/monitor.c",
            "third_party/glfw-3.3.8/src/vulkan.c",
            "third_party/glfw-3.3.8/src/window.c",
            "third_party/glfw-3.3.8/src/posix_time.c",
            "third_party/glfw-3.3.8/src/posix_thread.c",
            "third_party/glfw-3.3.8/src/xkb_unicode.c",
            "third_party/glfw-3.3.8/src/glx_context.c",
            "third_party/glfw-3.3.8/src/egl_context.c",
            "third_party/glfw-3.3.8/src/osmesa_context.c",
            "third_party/glfw-3.3.8/src/linux_joystick.c"
        )
        add_defines("_GLFW_X11")
        add_syslinks("X11", "Xrandr", "Xinerama", "Xcursor", "pthread", "dl")
    end

-- ========================
-- 2. GLAD（OpenGL 函数加载器）
-- ========================
target("glad_local")
    set_kind("static")
    set_group("third_party")

    add_includedirs("third_party/glad/include", {public = true})
    add_files("third_party/glad/src/glad.c")

-- ========================
-- 3. tinyfiledialogs（系统文件对话框）
-- ========================
-- 纯 C 文件，单独编译为静态库，避免与 C++ 预编译头（PCH）冲突
-- （PCH 包含 C++ 头文件，C 文件无法使用同一份 PCH）
target("tinyfiledialogs_local")
    set_kind("static")
    set_group("third_party")

    add_includedirs("third_party/tinyfiledialogs", {public = true})
    add_files("third_party/tinyfiledialogs/tinyfiledialogs.c")

-- ========================
-- 4. ImGui（即时模式 GUI 库）
-- ========================
target("imgui_local")
    set_kind("static")
    set_group("third_party")
    set_warnings("none")

    add_includedirs("third_party/imgui", {public = true})
    add_includedirs("third_party/imgui/backends", {public = true})

    -- ImGui 核心文件
    add_files("third_party/imgui/imgui.cpp")
    add_files("third_party/imgui/imgui_demo.cpp")   -- 演示窗口，debug 时可查看所有控件
    add_files("third_party/imgui/imgui_draw.cpp")
    add_files("third_party/imgui/imgui_tables.cpp")
    add_files("third_party/imgui/imgui_widgets.cpp")

    -- ImGui 后端（GLFW 负责输入，OpenGL3 负责渲染）
    add_files("third_party/imgui/backends/imgui_impl_glfw.cpp")
    add_files("third_party/imgui/backends/imgui_impl_opengl3.cpp")

    -- add_deps：声明依赖关系，会自动传递头文件路径和链接库
    add_deps("glfw_local", "glad_local")

-- ========================
-- 5. FluxPlayer 主程序
-- ========================
target("FluxPlayer")
    set_kind("binary")  -- 编译为可执行文件

    -- src/*.cpp 匹配根目录下的 main.cpp
    -- src/**/*.cpp 匹配所有子目录下的 .cpp 文件
    add_files("src/*.cpp")
    add_files("src/**/*.cpp")

    -- 声明依赖的静态库（会自动传递头文件路径）
    add_deps("glfw_local", "glad_local", "imgui_local", "tinyfiledialogs_local")

    -- 预编译头（PCH）：把常用的 STL 头和 Logger.h 预先编译好，
    -- 后续每个 .cpp 直接复用，避免重复解析，显著加快编译速度
    set_pcxxheader("include/FluxPlayer/pch.h")

    -- Unity Build：把多个 .cpp 合并成一个大文件一起编译，
    -- 减少重复解析头文件的开销，batchsize=8 表示每组合并 8 个文件
    -- OpenGL 相关文件排除在外，因为 glad 和系统 GL 头不能在同一编译单元中重复包含
    add_rules("c++.unity_build", {batchsize = 8})
    add_files("src/renderer/GLRenderer.cpp", {unity_ignored = true})
    add_files("src/renderer/Shader.cpp", {unity_ignored = true})
    add_files("src/ui/Window.cpp", {unity_ignored = true})
    add_files("src/ui/HomeScreen.cpp", {unity_ignored = true})

    -- 项目头文件搜索路径
    add_includedirs("include")          -- 项目自身头文件
    add_includedirs("third_party/glm")  -- GLM 数学库（仅头文件）

    -- FFmpeg：添加头文件路径、库搜索路径，然后链接各模块
    add_includedirs(ffmpeg_root .. "/include")
    add_linkdirs(ffmpeg_root .. "/lib")
    add_links("avformat", "avcodec", "avutil", "swscale", "swresample")

    -- 平台相关的系统库
    if is_plat("macosx") then
        -- add_frameworks：链接 macOS 系统框架（等价于 -framework xxx）
        add_frameworks("OpenGL", "Cocoa", "CoreVideo", "IOKit", "CoreFoundation", "AudioToolbox")
    elseif is_plat("windows") then
        -- add_syslinks：链接系统库（等价于 -l xxx 或 xxx.lib）
        add_syslinks("opengl32", "gdi32", "winmm", "ole32", "comdlg32", "d3d11", "dxgi")
    elseif is_plat("linux") then
        add_syslinks("GL", "X11", "pthread", "dl", "asound")  -- asound = ALSA 音频
    end

    -- Debug 模式：保留调试符号，关闭优化，定义 DEBUG 宏
    -- Release 模式：最高优化，去除符号表（减小文件体积）
    if is_mode("debug") then
        add_defines("DEBUG")
        set_symbols("debug")
        set_optimize("none")
    else
        set_optimize("fastest")
        set_strip("all")
    end

    -- TCP 日志开关（通过 xmake f --tcp_log=y 开启）
    if get_config("tcp_log") then
        add_defines("ENABLE_TCP_LOG")
    end

    -- 构建完成后自动执行的脚本
    after_build(function (target)
        -- 把着色器文件复制到可执行文件旁边（运行时需要读取）
        os.cp("assets/shaders", path.join(target:targetdir(), "shaders"))
        -- Windows：把 FFmpeg 的 DLL 复制到可执行文件旁边，否则运行时找不到
        if is_plat("windows") then
            local ffmpeg_bin = path.join(os.projectdir(), "third_party", "ffmpeg", "bin")
            if os.isdir(ffmpeg_bin) then
                os.cp(path.join(ffmpeg_bin, "*.dll"), target:targetdir())
            end
        end
    end)

    -- 加载时打印提示信息
    on_load(function (target)
        print("FluxPlayer - FFmpeg + OpenGL 跨平台播放器")
        print("使用 'xmake' 编译")
        print("使用 'xmake run' 运行程序")
    end)
