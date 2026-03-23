# FluxPlayer

**基于 FFmpeg 和 OpenGL 的现代化跨平台媒体播放器**

![Version](https://img.shields.io/badge/version-2.3-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![C++](https://img.shields.io/badge/C%2B%2B-17-orange.svg)

## 项目简介

FluxPlayer 是一个使用现代 C++17 开发的专业级视频播放器，采用 FFmpeg 进行音视频解码，OpenGL 硬件加速渲染，ImGui 提供现代化 UI 界面。实现了 VLC/MPV 级别的两阶段精确跳转，提供流畅的用户体验。

## ✨ 核心特性

- 🎬 **专业播放引擎** - 精确音视频同步（±1ms），流畅 60 FPS 播放
- ⚡ **硬件加速渲染** - OpenGL 4.1+ 着色器 YUV→RGB 转换
- 🎯 **精确跳转** - 两阶段跳转技术（<50ms 快速响应 + 0.1秒精度）
- 🎨 **现代化 UI** - ImGui 界面，播放控制、进度条、音量调节
- 🔊 **高质量音频** - macOS AudioQueue 原生支持，动态缓冲优化
- 📊 **信息面板** - 实时 FPS、媒体信息、统计数据
- 🎹 **快捷键控制** - 完整的键盘快捷键支持
- 🌍 **跨平台** - 支持 macOS、Windows（Linux 待测试）

## 技术栈

| 类别 | 技术 |
|------|------|
| 语言 | C++17 |
| 构建 | xmake |
| 音视频 | FFmpeg 4.x/5.x/6.x |
| 渲染 | OpenGL 4.1+ |
| 窗口 | GLFW 3.3.8 |
| UI | ImGui 1.90+ |

## 快速开始

### 安装依赖

**macOS:**
```bash
# 安装 xmake 和 FFmpeg
brew install xmake ffmpeg
```

**注意**: GLFW、GLAD、GLM 和 ImGui 已包含在 `third_party/` 目录中。

### 编译运行

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

# 播放不同格式的视频
xmake run FluxPlayer movie.mkv
xmake run FluxPlayer clip.avi

# 使用绝对路径
xmake run FluxPlayer /path/to/your/video.mp4

# 直接运行可执行文件
./build/macosx/arm64/release/FluxPlayer video.mp4
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

## 开发

### 编译模式

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
