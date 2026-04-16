-- xmake.lua
add_rules("mode.debug", "mode.release")

-- 项目信息
set_project("FluxPlayer")
set_version("0.1.0")
set_languages("cxx17")

-- TCP日志选项（用 nc ip port 远程查看实时日志）
option("tcp_log")
    set_default(false)
    set_showmenu(true)
    set_description("Enable TCP log server for remote log viewing")
option_end()

-- ========================
-- 配置路径
-- ========================
local ffmpeg_root = ""
if is_plat("windows") then
    ffmpeg_root = "third_party/ffmpeg"
elseif is_plat("macosx") then
    ffmpeg_root = "/opt/homebrew/opt/ffmpeg@4"
else
    ffmpeg_root = "/usr/local"
end

-- ========================
-- 1. GLFW 库（本地第三方库）
-- ========================
target("glfw_local")
    set_kind("static")
    set_group("third_party")
    set_warnings("none")

    add_includedirs("third_party/glfw-3.3.8/include", {public = true})

    if is_plat("macosx") then
        add_files("third_party/glfw-3.3.8/src/*.m", {mflags = "-fno-objc-arc"})
        add_files("third_party/glfw-3.3.8/src/*.c")
        add_files("third_party/glfw-3.3.8/src/posix_thread.c")

        -- 排除其他平台文件
        remove_files("third_party/glfw-3.3.8/src/win32_*.c")
        remove_files("third_party/glfw-3.3.8/src/wgl_*.c")
        remove_files("third_party/glfw-3.3.8/src/wl_*.c")
        remove_files("third_party/glfw-3.3.8/src/x11_*.c")
        remove_files("third_party/glfw-3.3.8/src/posix_time.c")
        remove_files("third_party/glfw-3.3.8/src/null_*.c")
        remove_files("third_party/glfw-3.3.8/src/linux_*.c")
        remove_files("third_party/glfw-3.3.8/src/xkb_*.c")
        remove_files("third_party/glfw-3.3.8/src/glx_*.c")

        add_mxflags("-fno-objc-arc")
        add_frameworks("Cocoa", "CoreVideo", "IOKit")
        add_defines("_GLFW_COCOA")
    elseif is_plat("windows") then
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
-- 2. GLAD 库（本地第三方库）
-- ========================
target("glad_local")
    set_kind("static")
    set_group("third_party")

    add_includedirs("third_party/glad/include", {public = true})
    add_files("third_party/glad/src/glad.c")

-- ========================
-- 3. ImGui 库（本地第三方库）
-- ========================
target("imgui_local")
    set_kind("static")
    set_group("third_party")
    set_warnings("none")

    add_includedirs("third_party/imgui", {public = true})
    add_includedirs("third_party/imgui/backends", {public = true})

    -- ImGui 核心文件
    add_files("third_party/imgui/imgui.cpp")
    add_files("third_party/imgui/imgui_demo.cpp")
    add_files("third_party/imgui/imgui_draw.cpp")
    add_files("third_party/imgui/imgui_tables.cpp")
    add_files("third_party/imgui/imgui_widgets.cpp")

    -- ImGui 后端（GLFW + OpenGL3）
    add_files("third_party/imgui/backends/imgui_impl_glfw.cpp")
    add_files("third_party/imgui/backends/imgui_impl_opengl3.cpp")

    -- 依赖 GLFW 和 GLAD
    add_deps("glfw_local", "glad_local")

-- ========================
-- 4. 主程序目标
-- ========================
target("FluxPlayer")
    set_kind("binary")
    add_files("src/**/*.cpp")
    add_files("src/main.cpp")
    add_files("third_party/tinyfiledialogs/tinyfiledialogs.c")

    -- 依赖本地库
    add_deps("glfw_local", "glad_local", "imgui_local")

    -- 包含头文件
    add_includedirs("include")
    add_includedirs("third_party/glfw-3.3.8/include")
    add_includedirs("third_party/glad/include")
    add_includedirs("third_party/glm")
    add_includedirs("third_party/tinyfiledialogs")

    -- FFmpeg 头文件和库
    add_includedirs(ffmpeg_root .. "/include")
    add_linkdirs(ffmpeg_root .. "/lib")
    add_links("avformat", "avcodec", "avutil", "swscale", "swresample")

    -- 平台相关配置
    if is_plat("macosx") then
        add_frameworks("OpenGL", "Cocoa", "CoreVideo", "IOKit", "CoreFoundation", "AudioToolbox")
    elseif is_plat("windows") then
        add_syslinks("opengl32", "gdi32", "winmm", "ole32", "comdlg32", "d3d11", "dxgi")
    elseif is_plat("linux") then
        add_syslinks("GL", "X11", "pthread", "dl")
    end

    -- 调试定义
    if is_mode("debug") then
        add_defines("DEBUG")
        set_symbols("debug")
        set_optimize("none")
    else
        set_optimize("fastest")
        set_strip("all")
    end

    -- TCP日志系统
    if get_config("tcp_log") then
        add_defines("ENABLE_TCP_LOG")
    end

    -- 运行时复制着色器文件和 FFmpeg DLL
    after_build(function (target)
        os.cp("assets/shaders", path.join(target:targetdir(), "shaders"))
        -- Windows: 复制 FFmpeg DLL 到可执行文件目录
        if is_plat("windows") then
            local ffmpeg_bin = path.join(os.projectdir(), "third_party", "ffmpeg", "bin")
            if os.isdir(ffmpeg_bin) then
                os.cp(path.join(ffmpeg_bin, "*.dll"), target:targetdir())
            end
        end
    end)

-- 提示信息
on_load(function (target)
    print("🎬 FluxPlayer - FFmpeg + OpenGL 跨平台播放器")
    print("📦 使用本地第三方库（GLFW、GLAD、GLM）")
    print("💡 使用 'xmake' 编译")
    print("💡 使用 'xmake run FluxPlayer' 运行程序")
end)
