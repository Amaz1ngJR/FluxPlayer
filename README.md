# FluxPlayer

![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Windows%20%7C%20Linux-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)

基于 FFmpeg + OpenGL 的跨平台视频播放器，使用 C++17 开发。支持本地文件播放和网络流（RTSP/RTMP/HTTP/HLS）播放，具备完整的音视频同步、GPU 渲染和 ImGui 控制界面。

## 功能特性

- 🎬 支持主流视频格式（MP4、MKV、AVI、FLV、MOV 等）
- 🌐 支持网络流播放（RTSP、RTMP、HTTP、HLS）
- 🖥️ OpenGL YUV→RGB GPU 渲染，GLSL 着色器实现色彩空间转换
- 🔊 跨平台音频输出（macOS AudioToolbox / Windows WinMM / Linux ALSA）
- 🎛️ ImGui 控制界面（播放控制、进度条、音量、媒体信息、统计面板）
- 📂 支持文件拖放打开
- ⏱️ 音视频同步（音频时钟 / 视频时钟 / 外部时钟三种模式）
- 🚀 FFmpeg 多线程解码，YUV420P 源帧零拷贝直通渲染
- ⚡ 硬件加速解码（macOS VideoToolbox / Windows CUDA(NVDEC)、D3D11VA、DXVA2），默认开启，自动降级
- 🎯 NV12 零拷贝渲染：硬件解码帧跳过 sws_scale，GL_RG8 纹理直通 GPU；CUDA 后端自动解交错兼容
- 📡 RTSP/RTMP/HLS 实时流 PTS 回绕检测与自动重校准，断流指数退避重试
- 🌊 网络流自适应缓冲：环形帧队列（对标 ffplay FrameQueue），本地 4 帧 / 网络 8 帧，预缓冲 5 帧起播
- 📊 实时统计信息（FPS、丢帧数、码率、队列深度）
- 📝 线程安全日志系统，支持 TCP 远程日志查看，运行时热更新日志级别
- ⚙️ INI 配置文件，支持热重载
- 📸 截图功能（PNG / JPEG，快捷键 P）
- 🎥 录像功能（转封装原始流，支持 low/medium/high/original 四档质量）
- 🎙️ 录音功能（自动适配 M4A / MKA 容器）
- 🔁 循环播放
- 💬 内嵌字幕流解码渲染（SRT / ASS / WebVTT / mov_text），ImGui 底部居中叠加，支持 CJK 字体自动探测

## 技术栈
| 组件 | 技术 |
|------|------|
| 语言 | C++17 |
| 视频解码 | FFmpeg 4.x / 6.x（自动适配），支持硬件加速 |
| 图形渲染 | OpenGL 3.3+ |
| 窗口管理 | GLFW 3.3.8 |
| UI | Dear ImGui |
| 数学库 | GLM |
| 构建系统 | xmake / CMake |

## 跨平台支持

| 平台 | 窗口系统 | 音频后端 | 状态 |
|------|---------|---------|------|
| macOS | Cocoa (GLFW) | AudioToolbox | ✅ 已支持，支持硬件加速 |
| Windows | Win32 (GLFW) | WinMM | ✅ 已支持 |
| Linux | X11 (GLFW) | ALSA | ✅ 已支持 |

## 项目结构

```
FluxPlayer/
├── src/                  # 源代码
│   ├── main.cpp
│   ├── audio/            # 音频输出 (AudioOutput)
│   ├── core/             # 播放器核心 (Player, AVSync, MediaInfo)
│   ├── decoder/          # 解码器 (Demuxer, VideoDecoder, AudioDecoder, Frame)
│   ├── recorder/         # 录制器 (Recorder)
│   ├── renderer/         # OpenGL 渲染 (GLRenderer, Shader)
│   ├── subtitle/         # 字幕模块 (SubtitleDecoder, SubtitleManager)
│   ├── ui/               # 界面 (Window, Controller, HomeScreen)
│   └── utils/            # 工具 (Config, Logger, Timer, Screenshot)
├── include/FluxPlayer/   # 头文件
├── assets/shaders/       # GLSL 着色器
├── docs/                 # 技术文档
├── third_party/          # GLFW, GLAD, ImGui, GLM, tinyfiledialogs
├── CMakeLists.txt
└── xmake.lua
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

FFmpeg 已集成在 `third_party/ffmpeg/` 中，无需额外安装。

使用 xmake 构建时，需要 MinGW-w64 编译器，运行 `setup_env.ps1` 初始化构建环境：

```powershell
# 首次使用需允许脚本执行（仅需一次）
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# 初始化 MinGW + xmake 环境
.\setup_env.ps1
```

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
# 安装 xmake 2.9.x：https://xmake.io
# 注意：需要 xmake 2.9.x，xmake 3.x 暂不兼容
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
| `F` | 全屏切换 |
| `←` / `→` | 后退 / 前进 16 秒 |
| `I` | 媒体信息 |
| `S` | 统计信息 |
| `H` | 强制切换 UI（默认鼠标移动自动显示/隐藏） |
| `P` | 截图（保存当前视频帧） |
| `Esc` | 退出 |

## UI 控制按钮

| 按钮 | 功能 |
|------|------|
| `Rec V` / `Stop V` | 开始 / 停止录像（录制中按钮变红，显示时长和文件大小） |
| `Rec A` / `Stop A` | 开始 / 停止录音（录制中按钮变红，显示时长和文件大小） |

## 支持格式

- 视频: H.264, H.265/HEVC, VP8, VP9, AV1
- 音频: AAC, MP3, Opus, PCM
- 容器: MP4, MKV, AVI, MOV, FLV, WebM

## 配置文件

程序首次运行时自动生成 `fluxplayer.ini`，后续修改值即可，切换界面时自动重载。

```ini
[Audio]
volume=0.6                    # 音量 (0.0 ~ 1.0)

[Log]
logLevel=INFO                 # 日志级别 (DEBUG / INFO / WARN / ERROR)
tcpLogPort=9999               # TCP 远程日志端口

[Window]
windowWidth=960               # 窗口默认宽度
windowHeight=600              # 窗口默认高度

[UI]
uiVisible=true                # 是否显示控制面板
showMediaInfo=true            # 是否显示媒体信息面板
showStats=true                # 是否显示统计信息面板

[Playback]
loopPlayback=false            # 是否循环播放

[Screenshot]
screenshotDir=Screenshot      # 截图保存目录
screenshotFormat=png          # 截图格式 (png / jpg)

[Record]
recordDir=Record              # 录制文件保存目录
recordQuality=original        # 录像质量 (low / medium / high / original)

[Decoder]
hwaccel=true                  # 硬件加速解码 (macOS: VideoToolbox / Windows: CUDA > D3D11VA > DXVA2)

[Subtitle]
subtitleEnabled=true          # 是否启用内嵌字幕流解码与渲染
subtitleFontScale=1.4         # 字幕字体缩放比例 (0.5 ~ 4.0)
subtitleFontPath=             # 自定义字体路径（留空则按平台自动探测 CJK 字体）
```

录像质量说明：
- `original`：直接转封装原始流，零质量损失
- `high`：H.264 重编码，8Mbps / CRF 18
- `medium`：H.264 重编码，4Mbps / CRF 23
- `low`：H.264 重编码，1Mbps / CRF 28

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
nc <播放器IP> 9999
```

## 测试流地址

> **提示**：公开测试流地址随时可能失效。如需稳定测试，建议用 FFmpeg 本地推流（见下方说明）。

### HTTP 点播（最稳定，推荐先用这些测试）

| 说明 | 地址 |
|------|------|
|||

### RTMP

| 说明 | 地址 |
|------|------|
| 伊拉克 Al Sharqiya 电视台 | `rtmp://ns8.indexforce.com/home/mystream` |

### HLS 直播/点播

| 说明 | 地址 |
|------|------|
| Apple 官方 HLS 测试流（HEVC，稳定） | `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_adv_example_hevc/master.m3u8` |
| Apple 官方 HLS 测试流（H.264） | `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_4x3/bipbop_4x3_variant.m3u8` |

### 本地推流测试（最可靠）

使用 FFmpeg + [MediaMTX](https://github.com/bluenviron/mediamtx) 搭建本地测试流：

```bash
# 1. 启动 MediaMTX（下载后直接运行即可）
./mediamtx

# 2. 用 FFmpeg 推送本地文件为 RTSP 流（循环播放）
ffmpeg -re -stream_loop -1 -i test.mp4 -c copy -f rtsp rtsp://localhost:8554/stream

# 3. 用 FluxPlayer 播放
./FluxPlayer rtsp://localhost:8554/stream
```

也可以推 RTMP 流：

```bash
ffmpeg -re -stream_loop -1 -i test.mp4 -c copy -f flv rtmp://localhost:1935/stream
```

## 技术要点

### 架构设计

- 多线程架构：解码线程 + 渲染线程 + 音频回调线程分离，通过线程安全的帧队列通信
- 模块解耦：Demuxer → Decoder → FrameQueue → Renderer / AudioOutput 流水线式处理
- 状态机管理：Player 通过 `PlayerState` 枚举管理 IDLE → OPENING → PLAYING → PAUSED → STOPPED 状态转换

### 视频渲染

- 双格式纹理支持：YUV420P（Y/U/V 三纹理）和 NV12（Y + UV 双纹理）
- GLSL 片段着色器通过 `isNV12` uniform 切换 YUV420P/NV12 采样路径
- NV12 UV 交错平面直接映射为 GL_RG8 纹理，零拷贝跳过 sws_scale
- 处理 FFmpeg linesize 与视频宽度不一致的内存对齐问题

### 硬件加速解码

- macOS: VideoToolbox（Apple Silicon / Intel Mac 专用媒体引擎）
- Windows: CUDA(NVDEC) → D3D11VA → DXVA2 按优先级自动选择
- 硬件帧通过 `av_hwframe_transfer_data()` 传输到 CPU（NV12 格式）
- 复用传输帧缓冲（`m_hwTransferFrame`），避免每帧 alloc/free
- CUDA 后端：`getHWFormat` 回调确保 FFmpeg 正确协商硬件像素格式；UV 解交错模式解决 GL_RG8 兼容性绿屏问题
- 全部候选失败时自动降级为软件解码，用户无感知
- 可通过 `hwaccel=false` 配置项强制关闭

### 音视频同步

- VSync 驱动渲染循环，基于主时钟 PTS 比较决定帧显示时机
- 本地文件使用外部时钟（系统时钟），实时流同样使用外部时钟
- 视频帧落后时连续丢帧追赶，无效 PTS 帧基于帧间隔估算补偿
- 音频帧部分消费残留缓冲，避免数据丢失导致播放速度异常

### 网络流处理

- 支持 RTSP / RTMP / HTTP / HLS 协议
- 实时流识别：URL 协议头检测 + HLS 格式名 + duration==0 多重判断，修复 RTMP 被解析为 FLV 格式名漏判
- 按协议设置专用选项：HLS 断流重连、RTSP 1MB 缓冲区、RTMP 直播模式
- 实时流 PTS 基准校准（音视频首帧 PTS 对齐）
- PTS 回绕检测：视频回绕时跳帧等待，音频回绕时统一重校准基准
- 无效 PTS（AV_NOPTS_VALUE）帧基于实际帧率 / 采样率估算 PTS，不丢弃
- 网络断流指数退避重试（100ms → 3000ms，最多 30 次）
- 网络流视频帧队列 8 帧 + 预缓冲 5 帧起播；音频帧队列 20 帧；背压机制防止欠载

### FFmpeg 版本兼容

- 通过 `LIBAVCODEC_VERSION_MAJOR` 宏自动适配 FFmpeg 4.x（channels）和 5.x+（ch_layout）API 差异
- `swr_alloc_set_opts` / `swr_alloc_set_opts2` 自动选择

