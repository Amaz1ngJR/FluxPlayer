# FluxPlayer

基于 FFmpeg + OpenGL 的跨平台媒体播放器，使用 C++17 开发，ImGui 提供现代化 UI。

> **平台支持**: macOS ✅ | Linux ✅ | Windows ✅（待测试）

## 特性

- 硬件加速 OpenGL 渲染，60fps VSync
- 两阶段精确跳转（关键帧快速响应 + 后台精确定位）
- 基于 PTS 的音视频同步（±1ms 精度）
- 跨平台音频输出（macOS: AudioToolbox / Windows: WinMM / Linux: ALSA）
- ImGui 界面：进度条、音量控制、媒体信息面板
- 内置彩色日志系统，支持控制台、文件和 TCP 远程输出
- 支持文件对话框选择视频文件

## 技术栈

| 组件 | 技术 |
|------|------|
| 语言 | C++17 |
| 视频解码 | FFmpeg 4.x / 6.x（自动适配） |
| 图形渲染 | OpenGL 3.3+ |
| 窗口管理 | GLFW 3.3.8 |
| UI | Dear ImGui |
| 数学库 | GLM |
| 构建系统 | xmake / CMake |

## 项目结构

```
FluxPlayer/
├── src/                  # 源代码
│   ├── main.cpp
│   ├── audio/            # 音频输出 (AudioOutput)
│   ├── core/             # 播放器核心 (Player, AVSync, MediaInfo)
│   ├── decoder/          # 解码器 (Demuxer, VideoDecoder, AudioDecoder, Frame)
│   ├── renderer/         # OpenGL 渲染 (GLRenderer, Shader)
│   ├── ui/               # 界面 (Window, Controller, HomeScreen)
│   └── utils/            # 工具 (Logger, Timer)
├── include/FluxPlayer/   # 头文件
├── assets/shaders/       # GLSL 着色器
├── third_party/          # GLFW, GLAD, ImGui, GLM, tinyfiledialogs
├── CMakeLists.txt
└── xmake.lua
```

## 环境依赖

### macOS

```bash
brew install ffmpeg@4
brew install xmake  # 或使用 CMake
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install -y build-essential cmake \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libasound2-dev
```

### Windows

1. 安装 [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)（选择"使用 C++ 的桌面开发"）
2. 安装 [CMake](https://cmake.org/download/) 或 [xmake](https://github.com/xmake-io/xmake/releases)
3. 下载 [FFmpeg 开发库](https://www.gyan.dev/ffmpeg/builds/)，解压到 `C:\ffmpeg`，将 `bin` 目录加入 PATH
4. 音频使用 WinMM API，无需额外安装

## 构建与运行

### xmake

```bash
xmake f -m release
xmake
xmake run FluxPlayer video.mp4
```

Windows 需先修改 `xmake.lua` 中的 FFmpeg 路径：
```lua
local ffmpeg_root = "C:/ffmpeg"
```

### CMake

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/FluxPlayer video.mp4
```

Windows 指定 FFmpeg 路径：
```bash
cmake -B build -DFFMPEG_ROOT="C:/ffmpeg"
```

## 快捷键

| 按键 | 功能 |
|------|------|
| `Space` | 播放 / 暂停 |
| `F` | 全屏切换 |
| `←` / `→` | 后退 / 前进 16 秒 |
| `I` | 媒体信息 |
| `S` | 统计信息 |
| `H` | 显示 / 隐藏 UI |
| `Esc` | 退出 |

## 支持格式

- 视频: H.264, H.265/HEVC, VP8, VP9, AV1
- 音频: AAC, MP3, Opus, PCM
- 容器: MP4, MKV, AVI, MOV, FLV, WebM

## TCP 远程日志

编译时启用：

```bash
# xmake
xmake f --tcp_log=y && xmake

# CMake
cmake -B build -DENABLE_TCP_LOG=ON && cmake --build build
```

使用 `nc` 查看实时日志：

```bash
nc 127.0.0.1 9999
```

## 架构

```
┌──────────────────────┐
│    UI Layer (ImGui)  │
│  播放控制 / 进度条    │
└──────────┬───────────┘
           │
┌──────────▼───────────┐
│   Core Layer         │
│  Player / AVSync     │
└──┬──────────────┬────┘
   │              │
┌──▼────┐    ┌───▼────┐
│Decoder│    │Renderer│
│FFmpeg │    │OpenGL  │
└───┬───┘    └────────┘
    │
┌───▼────┐
│ Audio  │
│ Output │
└────────┘
```
