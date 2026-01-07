# FluxPlayer

一个基于 FFmpeg 和 OpenGL 的现代化跨平台视频播放器。

> **平台支持状态**: 当前项目仅在 **macOS** 上开发并测试通过。Windows 和 Linux 平台的支持正在开发中，暂未经过完整测试。

## 项目简介

FluxPlayer 是一个轻量级、高性能的视频播放器，使用 C++17 开发，结合了 FFmpeg 的强大解码能力和 OpenGL 的硬件加速渲染，提供流畅的播放体验。

### 核心特性

- **跨平台设计**: 架构支持 macOS、Windows 和 Linux（当前仅 macOS 测试通过）
- **硬件加速渲染**: 使用 OpenGL 进行高性能视频渲染
- **音视频同步**: 精确的音视频同步机制
- **现代化 UI**: 基于 ImGui 的直观用户界面
- **丰富的控制功能**: 播放、暂停、快进、快退、全屏等
- **媒体信息显示**: 实时显示视频信息和播放统计

## 技术栈

- **编程语言**: C++17
- **视频解码**: FFmpeg 4.x
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

> **注意**: 以下配置仅在 macOS 上完整测试通过。Windows 和 Linux 平台的配置说明仅供参考，可能需要额外调整。

### 基础依赖

- **编译器**: 支持 C++17 的编译器
  - macOS: Clang 10+ (Xcode 12+) ✅ 已测试
  - Windows: MSVC 2019+ 或 MinGW-w64 ⚠️ 待测试
  - Linux: GCC 8+ 或 Clang 8+ ⚠️ 待测试
- **构建工具**: [xmake](https://xmake.io/) 或 [CMake](https://cmake.org/) 3.15+
- **FFmpeg**: 4.x 版本

### macOS (已测试 ✅)

```bash
# 安装 Homebrew (如果未安装)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装 FFmpeg 4
brew install ffmpeg@4

# 安装 xmake
brew install xmake
```

### Linux (Ubuntu/Debian) (待测试 ⚠️)

```bash
# 安装编译工具和依赖
sudo apt-get update
sudo apt-get install build-essential git cmake

# 安装 FFmpeg 开发库
sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev \
                     libswscale-dev libswresample-dev

# 安装 OpenGL 和 X11 开发库
sudo apt-get install libgl1-mesa-dev libx11-dev libxrandr-dev \
                     libxinerama-dev libxcursor-dev libxi-dev

# 安装 xmake
curl -fsSL https://xmake.io/shget.text | bash
```

### Windows (待测试 ⚠️)

1. 安装 [xmake](https://github.com/xmake-io/xmake/releases)
2. 下载并配置 [FFmpeg 开发库](https://ffmpeg.org/download.html)
3. 修改 `xmake.lua` 中的 `ffmpeg_root` 路径

## 构建与运行

> **当前状态**: 以下构建步骤在 **macOS** 上测试通过。其他平台可能需要调整配置。

项目支持两种构建系统：**xmake** 和 **CMake**，您可以选择其中一种。

### 方法 1: 使用 xmake 构建

#### 编译项目

```bash
# 克隆仓库
git clone <repository-url>
cd FluxPlayer

# 配置构建
xmake config -m release

# 编译
xmake build

# 运行播放器
xmake run FluxPlayer <video_file>

# 示例
xmake run FluxPlayer video.mp4
```

#### 调试模式

```bash
# 配置为调试模式
xmake config -m debug

# 编译并运行
xmake build
xmake run FluxPlayer <video_file>
```

### 方法 2: 使用 CMake 构建

#### macOS / Linux

```bash
# 克隆仓库
git clone <repository-url>
cd FluxPlayer

# 创建构建目录
mkdir build && cd build

# 配置项目（Release 模式）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 如果 FFmpeg 不在默认路径，需要指定 FFMPEG_ROOT
# macOS (Homebrew Intel):
# cmake .. -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT=/usr/local/opt/ffmpeg@4
# macOS (Homebrew Apple Silicon):
# cmake .. -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT=/opt/homebrew/opt/ffmpeg@4

# 编译（使用所有 CPU 核心）
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 运行播放器
./FluxPlayer <video_file>

# 示例
./FluxPlayer ../video.mp4
```

#### 调试模式

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
./FluxPlayer <video_file>
```

#### Windows (未测试)

```bash
# 使用 Visual Studio
mkdir build && cd build
cmake .. -DFFMPEG_ROOT="C:/path/to/ffmpeg"
cmake --build . --config Release

# 运行
Release\FluxPlayer.exe video.mp4
```

## 使用说明

### 键盘控制

启动播放器后，可以使用以下快捷键：

| 按键 | 功能 |
|-----|------|
| `SPACE` | 暂停/恢复播放 |
| `←` / `→` | 快退/快进 10 秒 |
| `F` | 切换全屏模式 |
| `ESC` | 退出播放器 |
| `I` | 显示/隐藏媒体信息面板 |
| `S` | 显示/隐藏统计信息面板 |
| `H` | 显示/隐藏 UI |

### 命令行参数

```bash
FluxPlayer <video_file>
```

- `video_file`: 要播放的视频文件路径（必需）

## 架构设计

FluxPlayer 采用模块化架构设计：

### 核心模块

- **Player**: 播放器核心，协调各个组件
- **AVSync**: 音视频同步管理
- **MediaInfo**: 媒体文件信息提取

### 解码模块

- **Demuxer**: 媒体文件解封装
- **VideoDecoder**: 视频流解码
- **AudioDecoder**: 音频流解码
- **Frame**: 帧数据管理

### 渲染模块

- **GLRenderer**: OpenGL 渲染器
- **Shader**: GLSL 着色器管理

### 音频模块

- **AudioOutput**: 音频输出管理

### UI 模块

- **Window**: 窗口管理
- **Controller**: UI 控制器

### 工具模块

- **Logger**: 日志系统
- **Timer**: 计时器

## 开发路线图

- [ ] 支持更多视频格式和编码
- [ ] 添加播放列表功能
- [ ] 实现字幕支持
- [ ] 添加视频滤镜效果
- [ ] 音频均衡器
- [ ] 截图功能
- [ ] 硬件解码支持 (VAAPI/VDPAU/VideoToolbox)
- [ ] 播放历史记录
- [ ] 配置文件支持

## 常见问题

### macOS 上找不到 FFmpeg

确保 FFmpeg 4 已正确安装，并且 `xmake.lua` 中的路径配置正确：

```bash
# 检查 FFmpeg 安装
brew list ffmpeg@4

# 确认安装路径
brew --prefix ffmpeg@4
```

### Linux 上缺少 OpenGL 库

安装必要的开发库：

```bash
sudo apt-get install libgl1-mesa-dev libglu1-mesa-dev
```

### 编译错误

如果遇到编译错误，尝试清理并重新构建：

```bash
xmake clean
xmake build
```

## 贡献指南

欢迎贡献代码、报告问题或提出建议！

