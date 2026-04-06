# FluxPlayer

基于 FFmpeg + OpenGL 的跨平台视频播放器，使用 C++17 开发。支持本地文件播放和网络流（RTSP/RTMP/HTTP/HLS）播放，具备完整的音视频同步、GPU 渲染和 ImGui 控制界面。

## 功能特性

- 🎬 支持主流视频格式（MP4、MKV、AVI、FLV、MOV 等）
- 🌐 支持网络流播放（RTSP、RTMP、HTTP、HLS）
- 🖥️ OpenGL YUV→RGB GPU 渲染，GLSL 着色器实现色彩空间转换
- 🔊 跨平台音频输出（macOS AudioToolbox / Windows WinMM / Linux ALSA）
- 🎛️ ImGui 控制界面（播放控制、进度条、音量、媒体信息、统计面板）
- 📂 支持文件拖放打开
- ⏱️ 音视频同步（音频时钟 / 视频时钟 / 外部时钟三种模式）
- 📊 实时统计信息（FPS、丢帧数、码率、队列深度）
- 📝 线程安全日志系统，支持 TCP 远程日志查看
- ⚙️ INI 配置文件，支持热重载

## 跨平台支持

| 平台 | 窗口系统 | 音频后端 | 状态 |
|------|---------|---------|------|
| macOS | Cocoa (GLFW) | AudioToolbox | ✅ 已支持 |
| Windows | Win32 (GLFW) | WinMM | ✅ 已支持 |
| Linux | X11 (GLFW) | ALSA | ✅ 已支持 |

## 项目结构

```
FluxPlayer/
├── include/FluxPlayer/       # 头文件
│   ├── core/                 # 核心模块（Player、AVSync、MediaInfo）
│   ├── decoder/              # 解码模块（Demuxer、VideoDecoder、AudioDecoder、Frame）
│   ├── renderer/             # 渲染模块（GLRenderer、Shader）
│   ├── audio/                # 音频输出（AudioOutput）
│   ├── ui/                   # UI 模块（Controller、HomeScreen、Window）
│   └── utils/                # 工具类（Logger、Config、Timer）
├── src/                      # 源文件（与头文件目录结构对应）
├── assets/shaders/           # GLSL 着色器（video.vert / video.frag）
├── third_party/              # 第三方库（源码编译）
│   ├── ffmpeg/               # FFmpeg（头文件 + 库文件）
│   ├── glfw-3.3.8/           # GLFW 窗口库
│   ├── glad/                 # OpenGL 加载器
│   ├── glm/                  # 数学库
│   ├── imgui/                # ImGui UI 库
│   └── tinyfiledialogs/      # 原生文件对话框
├── xmake.lua                 # xmake 构建配置
├── CMakeLists.txt            # CMake 构建配置
└── fluxplayer.ini.sample     # 配置文件示例
```

## 环境依赖

- C++17 编译器（GCC 8+ / Clang 10+ / MSVC 2019+）
- FFmpeg 4.x 或 5.x+（代码通过版本宏自动适配）
- OpenGL 3.3+

### macOS

```bash
brew install ffmpeg@4
```

### Windows

将 FFmpeg 开发包（include / lib / bin）放入 `third_party/ffmpeg/` 目录：

```
third_party/ffmpeg/
├── include/    # FFmpeg 头文件
├── lib/        # .lib 或 .a 静态库
└── bin/        # .dll 动态库（构建后自动复制到输出目录）
```

推荐从 [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) 或 [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) 下载预编译包。

### Linux

```bash
# Ubuntu / Debian
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libasound2-dev

# Fedora
sudo dnf install ffmpeg-devel alsa-lib-devel
```

## 构建

### 使用 xmake（推荐）

```bash
# 安装 xmake：https://xmake.io
# Release 构建
xmake

# Debug 构建
xmake f -m debug
xmake

# 启用 TCP 远程日志
xmake f --tcp_log=y
xmake

# 运行
xmake run FluxPlayer

# 播放指定文件
xmake run FluxPlayer /path/to/video.mp4

# 播放网络流
xmake run FluxPlayer rtsp://example.com/stream
```

### 使用 CMake

```bash
mkdir build && cd build

# macOS / Linux
cmake ..
make -j$(nproc)

# Windows (MinGW)
cmake -G "MinGW Makefiles" ..
mingw32-make -j

# 运行
./FluxPlayer
./FluxPlayer /path/to/video.mp4
```

## 使用方法

### 启动方式

- 无参数启动：进入 HomeScreen 主界面，可点击按钮选择文件、拖放文件或输入网络 URL
- 带参数启动：`FluxPlayer <文件路径或URL>` 直接播放

### 快捷键

| 快捷键 | 功能 |
|-------|------|
| `Space` | 播放 / 暂停 |
| `Escape` | 退出 |
| `Tab` | 显示 / 隐藏 UI 控制面板 |
| `I` | 显示 / 隐藏媒体信息 |
| `S` | 显示 / 隐藏统计信息 |

### 配置文件

复制 `fluxplayer.ini.sample` 为 `fluxplayer.ini` 并按需修改：

```ini
[Audio]
volume=0.6

[Log]
logLevel=INFO        # DEBUG / INFO / WARN / ERROR
tcpLogPort=9999

[Window]
windowWidth=960
windowHeight=600

[UI]
uiVisible=true
showMediaInfo=true
showStats=true
```

### TCP 远程日志

启用 `tcp_log` 编译选项后，可通过 netcat 远程查看实时日志：

```bash
nc <播放器IP> 9999
```

## 技术要点

### 架构设计

- 多线程架构：解码线程 + 渲染线程 + 音频回调线程分离，通过线程安全的帧队列通信
- 模块解耦：Demuxer → Decoder → FrameQueue → Renderer / AudioOutput 流水线式处理
- 状态机管理：Player 通过 `PlayerState` 枚举管理 IDLE → OPENING → PLAYING → PAUSED → STOPPED 状态转换

### 视频渲染

- YUV420P 三平面纹理上传（Y / U / V 独立纹理）
- GLSL 片段着色器实现 YUV→RGB 色彩空间转换
- 处理 FFmpeg linesize 与视频宽度不一致的内存对齐问题

### 音视频同步

- 默认以音频时钟为主时钟（Audio Master Clock）
- 视频帧根据 PTS 与音频时钟的差值动态调整延迟
- 支持自适应丢帧策略（阈值 40ms ~ 100ms）
- 音频缓冲延迟动态补偿

### 网络流处理

- 支持 RTSP / RTMP / HTTP / HLS 协议
- 实时流 PTS 基准校准（音视频首帧 PTS 对齐）
- 动态音频队列深度调整

### FFmpeg 版本兼容

- 通过 `LIBAVCODEC_VERSION_MAJOR` 宏自动适配 FFmpeg 4.x（channels）和 5.x+（ch_layout）API 差异
- `swr_alloc_set_opts` / `swr_alloc_set_opts2` 自动选择

