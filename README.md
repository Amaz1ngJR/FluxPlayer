# FluxPlayer

**基于 FFmpeg 和 OpenGL 的现代化跨平台媒体播放器**

> **平台支持状态**:
> - **macOS**: ✅ 已完成开发和测试
> - **Windows**: ⚠️ 已完成开发（使用 WinMM 音频 API），待测试
> - **Linux**: ✅ 已完成开发和测试

## 项目简介

FluxPlayer 是一个使用现代 C++17 开发的专业级视频播放器，采用 FFmpeg 进行音视频解码，OpenGL 硬件加速渲染，ImGui 提供现代化 UI 界面。实现了 VLC/MPV 级别的两阶段精确跳转，提供流畅的用户体验。

## ✨ 核心特性

- **跨平台设计**: 支持 macOS 和 Windows（Linux 正在开发中）
- **硬件加速渲染**: 使用 OpenGL 进行高性能视频渲染
- **音视频同步**: 精确的音视频同步机制
- **跨平台音频输出**:
  - macOS: AudioToolbox/CoreAudio
  - Windows: WinMM API
  - Linux: ALSA/PulseAudio (开发中)
- **现代化 UI**: 基于 ImGui 的直观用户界面
- **丰富的控制功能**: 播放、暂停、快进、快退、全屏等
- **媒体信息显示**: 实时显示视频信息和播放统计

## 技术栈

- **编程语言**: C++17
- **视频解码**: FFmpeg 4.x / 6.x（通过条件编译自动适配）
- **图形渲染**: OpenGL 3.3+
- **窗口管理**: GLFW 3.3.8
- **OpenGL 加载器**: GLAD
- **用户界面**: Dear ImGui
- **数学库**: GLM
- **构建系统**: xmake / CMake

## 项目结构

```
FluxPlayer/
├── assets/              # 资源文件
│   └── shaders/        # GLSL 着色器
├── docs/               # 文档
├── include/            # 公共头文件
│   └── FluxPlayer/
│       ├── audio/      # 音频输出
│       ├── core/       # 核心播放器逻辑
│       ├── decoder/    # 解码器
│       ├── renderer/   # 渲染器
│       ├── ui/         # 用户界面
│       └── utils/      # 工具类
├── src/                # 源代码实现
├── third_party/        # 第三方依赖库
│   ├── glfw-3.3.8/    # GLFW 窗口库
│   ├── glad/          # OpenGL 加载器
│   ├── imgui/         # ImGui UI 库
│   └── glm/           # 数学库
├── CMakeLists.txt     # CMake 构建配置
└── xmake.lua          # xmake 构建配置
```

## 环境要求

### 基础依赖

- **编译器**: 支持 C++17 的编译器
  - macOS: Clang 10+ (Xcode 12+) ✅ 已测试
  - Windows: MSVC 2019+ 或 MinGW-w64 ⚠️ 待测试
  - Linux: GCC 8+ 或 Clang 8+ ✅ 已测试（Orange Pi 5 / Ubuntu 22.04）
- **构建工具**: [xmake](https://xmake.io/) 或 [CMake](https://cmake.org/) 3.15+
- **FFmpeg**: 4.x 或 6.x 版本（通过条件编译自动适配）

### macOS (已测试 ✅)

```bash
# 安装 Homebrew (如果未安装)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装 FFmpeg 4
brew install ffmpeg@4

# 安装 xmake
brew install xmake
```

### Linux (Ubuntu/Debian) ✅

```bash
# 安装编译工具和依赖
sudo apt-get update
sudo apt-get install -y build-essential git cmake

# 安装 FFmpeg 开发库
sudo apt-get install -y libavcodec-dev libavformat-dev libavutil-dev \
                        libswscale-dev libswresample-dev

# 安装 OpenGL 和 X11 开发库（GLFW 编译必需）
sudo apt-get install -y libgl1-mesa-dev libx11-dev libxrandr-dev \
                        libxinerama-dev libxcursor-dev libxi-dev

# 安装音频库（ALSA）
sudo apt-get install -y libasound2-dev

# 安装 xmake（可选）
curl -fsSL https://xmake.io/shget.text | bash
```

### Windows (已支持 ✅)

#### 使用 VSCode 开发（推荐）

1. **安装开发工具**
   - 安装 [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) 或完整的 Visual Studio
     - 只需选择"使用 C++ 的桌面开发"工作负载
   - 安装 [VSCode](https://code.visualstudio.com/)
   - 安装构建工具：[CMake](https://cmake.org/download/) 和/或 [xmake](https://github.com/xmake-io/xmake/releases)

2. **安装 VSCode 扩展**
   - C/C++ (Microsoft)
   - CMake Tools (Microsoft) - 如果使用 CMake
   - xmake (tboox) - 如果使用 xmake

3. **安装 FFmpeg**
   - 下载 [FFmpeg 开发库](https://www.gyan.dev/ffmpeg/builds/)
     - 选择 "ffmpeg-release-full-shared.7z" 或 "ffmpeg-release-full.7z"
   - 解压到目标路径，例如 `C:\ffmpeg`
   - 将 FFmpeg 的 `bin` 目录添加到系统 PATH 环境变量
   - 修改构建配置中的 FFmpeg 路径：
     - **xmake**: 编辑 `xmake.lua` 中的 `ffmpeg_root = "C:/ffmpeg"`
     - **CMake**: 使用参数 `-DFFMPEG_ROOT="C:/ffmpeg"`

4. **音频支持**: Windows 版本使用 WinMM API，系统自带无需额外安装

#### 使用 MSYS2 + MinGW（可选）

如果你更喜欢类 Unix 环境：

```bash
# 安装 MSYS2: https://www.msys2.org/
# 在 MSYS2 终端中执行：
pacman -S mingw-w64-x86_64-toolchain
pacman -S mingw-w64-x86_64-ffmpeg
pacman -S mingw-w64-x86_64-cmake
pacman -S mingw-w64-x86_64-xmake
```

## 构建与运行

项目支持两种构建系统：**xmake** 和 **CMake**，您可以选择其中一种。

### 方法 1: 使用 xmake 构建

#### macOS / Linux

```bash
# 克隆项目
git clone <repository-url>
cd FluxPlayer

# 编译（Release 模式）
xmake f -m release
xmake

# 运行
xmake run FluxPlayer /path/to/video.mp4
```

## 使用指南

### 播放视频

```bash
# 基本用法
xmake run FluxPlayer video.mp4

# 编译并运行
xmake build
xmake run FluxPlayer <video_file>
```

#### Windows

```bash
# 克隆仓库
git clone <repository-url>
cd FluxPlayer

# 修改 xmake.lua 中的 FFmpeg 路径
# local ffmpeg_root = "C:/ffmpeg"

# 配置构建
xmake config -m release

# 编译
xmake build

# 运行播放器
xmake run FluxPlayer <video_file>

# 示例
xmake run FluxPlayer video.mp4
```

### 键盘快捷键

| 按键 | 功能 |
|------|------|
| `SPACE` | 播放/暂停 |
| `ESC` | 退出 |
| `F` | 全屏切换 |
| `←` / `→` | 后退/前进 16 秒 |
| `I` | 切换媒体信息 |
| `S` | 切换统计信息 |
| `H` | 切换 UI 显示 |

### UI 控制

- **进度条** - 点击或拖动跳转，悬停预览目标时间
- **播放控制** - 播放/暂停/停止按钮
- **音量控制** - 实时音量调节和静音
- **信息面板** - 媒体信息、FPS 统计

### 支持格式

- **视频**: H.264, H.265/HEVC, VP8, VP9, AV1
- **音频**: AAC, MP3, Opus, PCM
- **容器**: MP4, MKV, AVI, MOV, FLV, WebM

## 核心技术

### 两阶段精确跳转

FluxPlayer 采用专业级两阶段跳转方案，与主流播放器同等水平：

**阶段 1：快速响应**
- 跳转到最近关键帧并立即显示
- 响应时间 <50ms
- 用户立即看到画面变化

**阶段 2：精确定位**
- 后台解码到目标位置
- 快速丢弃中间帧
- 0.1秒精度定位

**性能优化**
- 跳过不必要的格式转换
- 减少锁竞争
- 使用 `avformat_seek_file` 精确寻址

### 音视频同步

- 基于 PTS 时间戳的音频时钟同步
- 同步精度 ±1ms
- 动态帧率调整
- 智能丢帧策略

### 架构设计

```
┌──────────────────────┐
│    UI Layer (ImGui)  │
│  播放控制 / 进度条    │
└──────────┬───────────┘
           │
┌──────────▼─────���─────┐
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
│ Queue  │
└────────┘
```

## 性能指标

| 指标 | 数值 |
|------|------|
| FPS | 60 (VSync) |
| 内存占用 | ~50 MB |
| 二进制大小 | ~280 KB |
| 跳转响应 | <50 ms |
| 跳转精度 | 0.1 秒 |
| 同步精度 | ±1 ms |

## 项目结构

```
FluxPlayer/
├── src/                # 源代码
│   ├── core/          # 播放器核心
│   ├── decoder/       # FFmpeg 解码
│   ├── renderer/      # OpenGL 渲染
│   ├── audio/         # 音频输出
│   ├── ui/            # UI 控制
│   └── utils/         # 工具类
├── include/           # 头文件
├── third_party/       # 第三方库
├── assets/            # 着色器资源
└── docs/              # 文档
```

#### Windows

**使用命令行：**

```bash
# Debug 模式（带调试信息）
xmake f -m debug
xmake

# Release 模式（优化性能）
xmake f -m release
xmake
```

### 日志系统

FluxPlayer 内置了完整的彩色日志系统，支持控制台、文件和 TCP 远程输出。

#### 基本用法

```cpp
#include "FluxPlayer/utils/Logger.h"

// 使用便捷宏输出不同级别的日志
LOG_DEBUG("This is a debug message");    // 灰色 - 调试信息
LOG_INFO("This is an info message");     // 绿色 - 一般信息
LOG_WARN("This is a warning message");   // 黄色 - 警告信息
LOG_ERROR("This is an error message");   // 红色 - 错误信息
```

#### 日志配置

```cpp
using namespace FluxPlayer;

// 设置最低日志级别
Logger::getInstance().setLogLevel(LogLevel::LOG_INFO);

// 启用文件输出
Logger::getInstance().enableFileOutput("fluxplayer.log");

// 禁用文件输出
Logger::getInstance().disableFileOutput();
```

#### TCP 远程日志（可选）

适用于无法直接查看终端的场景（如嵌入式设备、远程服务器）。

**编译时启用：**

```bash
# xmake
xmake f --tcp_log=y
xmake

# CMake
cmake -B build -DENABLE_TCP_LOG=ON
cmake --build build
```

**使用 nc 查看实时日志：**

```bash
# 连接到日志服务器（默认端口 9999）
nc 127.0.0.1 9999

# 远程查看
nc <server_ip> 9999
```

程序启动后会自动开启 TCP 日志服务器，支持多客户端同时连接，实时接收带颜色的日志输出。

#### 日志格式

控制台和文件输出格式：
```
[2026-01-07 15:30:45] [INFO ] Player created
[2026-01-07 15:30:45] [WARN ] Audio buffer underrun
[2026-01-07 15:30:45] [ERROR] Failed to open file
```

#### 颜色方案

- 🔘 **DEBUG** - 灰色 (`\033[90m`)
- 🟢 **INFO** - 绿色 (`\033[32m`)
- 🟡 **WARN** - 黄色 (`\033[33m`)
- 🔴 **ERROR** - 红色 (`\033[31m`)
