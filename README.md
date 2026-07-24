# FluxPlayer

![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Windows%20%7C%20Linux-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)

<div align="center">
  <img src="source/pic2.png" width="720" alt="FluxPlayer" />
</div>


基于 FFmpeg + OpenGL 的跨平台桌面视频播放器，使用 C++17 开发，覆盖 macOS / Windows / Linux 三端。从本地文件、纯音频，到 RTSP/RTMP/HTTP/HLS/DASH 网络流，再到 B站、YouTube 等 1000+ 平台的网页视频，都能直接打开播放。

核心能力包括：OpenGL YUV→RGB GPU 渲染与色彩空间自适应、硬件加速解码（VideoToolbox / CUDA / D3D11VA / DXVA2）及 NV12 零拷贝直通、自适应主时钟音视频同步、内嵌字幕解码渲染，以及截图（含 YUV/NV12 原始格式）、图片查看（JPG/PNG/YUV/NV12）、录像录音、多视频合并与片段截取、网页视频下载（断点续传）、画质切换、倍速播放等实用功能。界面由 Dear ImGui 构建，配套语义 token 驱动、支持热加载的皮肤系统。

## 功能特性

- 🎬 支持主流视频格式（MP4、MKV、AVI、FLV、MOV 等）
- 🌐 支持网络流播放（RTSP、RTMP、HTTP、HLS）
- 🖥️ OpenGL YUV→RGB GPU 渲染，自适应 BT.601/BT.709/BT.2020 色彩空间 + TV/PC 量化范围
- 🔊 跨平台音频输出（macOS AudioToolbox / Windows WinMM / Linux ALSA）
- 🎛️ ImGui 控制界面（播放控制、进度条、音量、媒体信息、统计面板）
- 📂 支持文件拖放打开
- ⏱️ 音视频同步（自适应主时钟：有音频用音频时钟、无音频退回外部时钟），音频时钟回调间隙实时插值消除阶梯跳变
- 🚀 FFmpeg 多线程解码，YUV420P 源帧零拷贝直通渲染
- ⚡ 硬件加速解码（macOS VideoToolbox / Windows CUDA(NVDEC)、D3D11VA、DXVA2），默认开启，自动降级
- 🎯 NV12 零拷贝渲染：硬件解码帧跳过 sws_scale，GL_RG8 纹理直通 GPU；CUDA 后端自动解交错兼容
- 📡 RTSP/RTMP/HLS 实时流 PTS 回绕检测与自动重校准，断流指数退避重试
- 🌊 网络流自适应缓冲：解耦的 PacketQueue（serial 序号 + 字节/时长背压）+ 环形帧队列（对标 ffplay），本地 4 帧 / 网络 8 帧，预缓冲 5 帧起播
- 📊 实时统计信息（FPS、丢帧数、码率、队列深度）
- 📝 线程安全日志系统，支持 TCP 远程日志查看，运行时热更新日志级别
- ⚙️ INI 配置文件，支持热重载
- 📸 截图功能（PNG / JPEG / YUV(I420) / NV12，快捷键 P）
- 🖼️ 图片查看（JPG / PNG / YUV I420 / NV12，拖放或文件对话框打开）
- 🎥 录像功能（纯转封装原始流，无损保留画质，零重编码开销）
- 🎙️ 录音功能（自动适配 M4A / MKA 容器）
- 🎬 多视频合并与片段截取（主页 MERGE VIDEOS 入口）：多选/拖放添加、拖拽调序、单项删除；每个片段可设 IN/OUT 截取范围并实时预览入点/出点画面；同一文件可多次添加各取一段；智能模式：全整段且参数一致走流拷贝极速无损，含截取或参数不一致时统一转码 H.264/AAC 帧级精确；输出到录制目录
- 🔁 循环播放
- 🕒 观看历史（主页右侧侧栏）：本地视频/音频与网页视频自动记录，点击即重播；LRU 上限 10 条（最近置顶，超限淘汰最久），支持单条删除与一键清空；持久化到 `history.json`，关闭重开仍在
- ⏩ 播放速度控制（0.5x / 0.75x / 1.0x / 1.25x / 1.5x / 2.0x），音频最近邻重采样变速
- 💬 内嵌字幕流解码渲染（SRT / ASS / WebVTT / mov_text），ImGui 底部居中叠加，支持 CJK 字体自动探测
- 🌍 网页视频播放（B站、YouTube 等 1000+ 平台），自动提取真实流地址，支持 DASH 分离流合并
- 🍪 内置浏览器登录窗口（WebView2 / WKWebView），Cookie 由程序自动维护，无需读取系统浏览器
- 📥 视频下载功能，支持进度/速度/文件大小/ETA 显示，可暂停/恢复/取消；网络中断自动指数退避重连续传，下载中使用 `.part` 临时文件，完成后重命名，取消/失败/崩溃自动清理
- 🎯 画质切换（360P / 480P / 720P / 1080P），切换时保持播放位置
- 🔒 网络代理支持（HTTP/SOCKS5），默认 127.0.0.1:7890，可配置开关
- 🎨 皮肤系统（Skin System）：语义 token 驱动的 UI 主题，支持 JSON 皮肤包加载、三层搜索（用户/开发/内置）、热加载（文件变更自动刷新，无需重启）、Appearance 子页切换皮肤，默认内置 `cyberpunk-neon`

<div align="center">
  <img src="source/UI/skins/cyberpunk-neon/mockup_player.svg" width="720" alt="播放器界面预览" />
  <br/>
  <img src="source/UI/skins/cyberpunk-neon/mockup_skin_settings.svg" width="720" alt="皮肤设置预览" />
</div>

## 技术栈
| 组件 | 技术 |
|------|------|
| 语言 | C++17 |
| 视频解码 | FFmpeg（macOS 4.x / Windows 7.x，通过版本宏自动适配），支持硬件加速 |
| 图形渲染 | OpenGL 3.3+ |
| 窗口管理 | GLFW 3.3.8 |
| UI | Dear ImGui |
| 数学库 | GLM |
| 构建系统 | xmake / CMake |

## 跨平台支持

| 平台 | 窗口系统 | 音频后端 | 状态 |
|------|---------|---------|------|
| macOS | Cocoa (GLFW) | AudioToolbox | ✅ 已支持，硬件加速 (VideoToolbox) |
| Windows | Win32 (GLFW) | WinMM | ✅ 已支持，硬件加速 (CUDA/D3D11VA/DXVA2) |
| Linux | X11 (GLFW) | ALSA | ✅ 已支持 |

## 项目结构

```
FluxPlayer/
├── src/                  # 源代码
│   ├── main.cpp
│   ├── audio/            # 音频输出 (AudioOutput)
│   ├── core/             # 播放器核心（Player 门面 + 命令队列 CommandQueue + 状态机 StateManager + 时钟 ClockController + 队列管理 QueueManager + 读包 DemuxWorker + 解码 DecodeWorker + 录制 RecordingService + AVSync / MediaInfo / FrameQueue / PacketQueue / PTSNormalizer）
│   ├── decoder/          # 解码器 (Demuxer, VideoDecoder, AudioDecoder, Frame)
│   ├── recorder/         # 录制器 (Recorder)
│   ├── renderer/         # OpenGL 渲染 (GLRenderer, Shader)
│   ├── subtitle/         # 字幕模块 (SubtitleDecoder, SubtitleManager)
│   ├── video/            # 视频后处理 (FrameInterpolator 帧插值)
│   ├── ui/               # 界面 (Window, Controller, HomeScreen, MergeScreen, OpeningScreen, UiContext, Skin/SkinManager/SkinRenderer 皮肤系统)
│   └── utils/            # 工具 (Config, Logger, Timer, Screenshot, StreamExtractor, CookieStore, WebLogin, DashMerger, VideoMerger, VideoFramePreviewer, Downloader, HistoryStore)
├── include/FluxPlayer/   # 头文件
├── assets/shaders/       # GLSL 着色器
├── docs/                 # 技术文档
├── third_party/          # GLFW, GLAD, ImGui, GLM, tinyfiledialogs, FFmpeg (Win/Mac)
├── scripts/              # 构建辅助脚本
├── CMakeLists.txt
└── xmake.lua
```

## 总体架构图

### 图一：控制流（命令队列）

UI 线程不直接修改播放器内部状态，所有操作（Seek、Pause、Stop 等）封装成命令投入 `CommandQueue`。控制线程（即主线程）在每帧渲染前调用 `pumpCommands()` 取出命令串行执行，再分发给各组件处理，从而彻底消除多线程并发修改状态的问题。

```plantuml
@startuml
skinparam backgroundColor #FFFFFF
skinparam defaultFontName "Microsoft YaHei,SimHei,Arial"
skinparam componentFontSize 14
skinparam noteFontSize 12
skinparam nodesep 60
skinparam ranksep 80

<style>
component {
    Padding 10
    Margin 5
}
</style>

package "UI 线程" as ui_thread #FFFDE7 {
    component "  Controller  " as ctrl
}

queue "  CommandQueue  " as cmdq #FFE0B2

package "控制线程" as ctrl_thread #E3F2FD {
    component "  pumpCommands()  " as pump
    component "  StateManager  " as state
    component "  ClockController  " as clock
    component "  QueueManager  " as qmgr
    component "  RecordingService  " as rec
    component "  GLRenderer  " as gl
}

ctrl -down-> cmdq : "  post(cmd)  "
cmdq -down-> pump : "  drain()  "

pump -down-> state
pump -down-> clock
pump -down-> qmgr
pump -down-> rec
pump -down-> gl

note right of ctrl
  用户交互
  ————————
  拖动进度条
  点击按钮
  快捷键
end note

note right of cmdq
  命令类型
  ————————
  SeekCommand
  PauseCommand
  ResumeCommand
  StopCommand
  SetSpeedCommand
  SwitchQualityCommand
end note

note bottom of pump
  串行执行保证
  ————————————————
  每帧渲染前统一处理命令
  消除多线程竞态条件
end note

@enduml
```

### 图二：数据流（三线程解耦）

媒体数据经由三条独立线程流转：**DemuxWorker** 从文件/网络读包分发到两条 PacketQueue；**DecodeWorker(Video/Audio)** 各自取包解码后放入 FrameQueue；主线程从 videoFrameQueue 取帧渲染，平台音频回调从 audioFrameQueue 取帧播放。视频和音频队列完全独立——视频队列堵住只会阻塞视频解码线程，音频可以持续产帧，从根本上消除了「视频卡顿导致音频饿死」的死锁。`serial` 序号用于 seek：每次 seek 时递增 serial，decode 线程检测到 serial 变化即丢弃旧包，画面立刻切换到新位置。

```plantuml
@startuml
skinparam backgroundColor #FFFFFF
skinparam defaultFontName "Arial"
skinparam ArrowColor #333333
skinparam ArrowThickness 2

storage "文件/网络流" as FileSource #LightBlue

package "线程1: DemuxWorker" #FFF3E0 {
    component [Demuxer\n解复用器] as dmx
    note right of dmx
      职责：
      • av_read_frame()
      • 音视频流分离
      • 录制无锁写入
    end note
}

queue "PacketQueue\n(压缩视频包)" as vpq #E8EAF6
queue "PacketQueue\n(压缩音频包)" as apq #E8EAF6

package "线程2: DecodeWorker (Video)" #E8F5E9 {
    component [VideoDecoder\n视频解码器] as vdec
    note right of vdec
      职责：
      • avcodec_send_packet()
      • avcodec_receive_frame()
      • 硬件加速 (NV12)
    end note
}

package "线程3: DecodeWorker (Audio)" #FFF9C4 {
    component [AudioDecoder\n音频解码器] as adec
    note right of adec
      职责：
      • avcodec_send_packet()
      • avcodec_receive_frame()
      • PCM 输出
    end note
}

queue "FrameQueue\n(YUV 视频帧)" as vfq #FFECB3
queue "FrameQueue\n(PCM 音频帧)" as afq #FFECB3

package "主线程: Renderer" #FCE4EC {
    component [GLRenderer\n渲染器] as gl
    component [AudioOutput\n音频输出] as ao
    component [AVSync\n同步器] as sync
}

actor "显示器\n扬声器" as Output #FF9800

FileSource -down-> dmx

dmx -down-> vpq : put(pkt, serial)
dmx -down-> apq : put(pkt, serial)

vpq -down-> vdec : get() 阻塞读取
apq -down-> adec : get() 阻塞读取

vdec -down-> vfq : push(YUV Frame)
adec -down-> afq : push(PCM Frame)

vfq -down-> gl : peekRef() 独立引用
afq -down-> ao : peekRef() 独立引用

gl -down-> sync
ao -down-> sync

sync -down-> Output

note bottom of vpq
  **serial 机制**：
  seek 时递增 serial
  decode 线程检测到变化
  立即 flush 解码器
end note

note bottom of vfq
  **独立队列消除死锁**：
  视频队列满 → 仅阻塞视频解码
  音频队列独立 → 持续产帧
  消除「视频饿死音频」
end note

note left of dmx
  **录制无锁**：
  writePacket() 全归
  DemuxWorker 线程
  串行执行
end note

@enduml
```

详细架构设计见 [`docs/v0.6.0命令队列与控制线程架构重构方案.md`](docs/v0.6.0命令队列与控制线程架构重构方案.md)。


## 环境依赖

- C++17 编译器（GCC 8+ / Clang 10+ / MSVC 2019+）
- OpenGL 3.3+
- FFmpeg 已在 macOS 和 Windows 上自包含，无需系统安装

> **FFmpeg 版本说明**：macOS 使用 FFmpeg 4.x（avcodec-58），Windows 使用 FFmpeg 7.x（avcodec-62），两个平台的头文件和动态库均不共用。源码通过 `LIBAVCODEC_VERSION_MAJOR` 宏自动适配 API 差异（如 `channels` vs `ch_layout`）。

### macOS

FFmpeg 4.x 已集成在 `third_party/ffmpeg-macos/` 中，无需额外安装。

如需重新生成（开发者）：

```bash
brew install ffmpeg@4
python3 scripts/bundle_ffmpeg_macos.py
```

### Windows

FFmpeg 7.x 已集成在 `third_party/ffmpeg/` 中，无需额外安装。

使用 xmake 构建时，需要 MinGW-w64 编译器，运行 `setup_env.ps1` 初始化构建环境：

```powershell
# 首次使用需允许脚本执行（仅需一次）
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# 初始化 MinGW + xmake 环境
.\setup_env.ps1
```

初始化脚本会把 xmake 构建根目录配置为 `build/windows`；主程序和运行时依赖仍按
`xmake.lua` 的约定输出到共享的 `build/bin`。

### Linux

```bash
# Ubuntu / Debian
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libasound2-dev

# Fedora
sudo dnf install ffmpeg-devel alsa-lib-devel
```

## 构建

### 构建目录约定

CMake 和 xmake 共用 `build/` 根目录，但中间文件严格隔离，最终运行产物统一输出：

```text
build/
├── cmake/                 # CMake 缓存、生成器文件、对象文件和静态库
├── windows/               # xmake 的缓存、对象文件和第三方静态库
└── bin/                   # 两者共用的 FluxPlayer、动态库和运行时资源
```

`build/bin` 是共享运行目录，后执行的构建会覆盖同名最终产物。CMake 与 xmake
不会共享中间文件，因此可以在两套构建系统之间切换而不污染增量编译缓存。

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

# Windows 最终运行产物
.\build\bin\FluxPlayer.exe
```

### 使用 CMake

```powershell
# Windows：中间文件进入 build\cmake，最终产物进入 build\bin
cmake -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\cmake

# 运行
.\build\bin\FluxPlayer.exe
.\build\bin\FluxPlayer.exe <视频文件路径>
```

```bash
# macOS / Linux
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake -j

# 运行
./build/bin/FluxPlayer
./build/bin/FluxPlayer /path/to/video.mp4
```

## 打包环境搭建

### macOS

所有工具均为系统自带，无需额外安装：

| 工具 | 用途 |
|------|------|
| `cmake` | 构建系统（Xcode Command Line Tools 自带） |
| `sips` | PNG 缩放生成各尺寸图标 |
| `iconutil` | `.iconset` 目录转 `.icns` |
| `hdiutil` | 打包 `.dmg` 磁盘镜像 |

如未安装 Xcode Command Line Tools：

```bash
xcode-select --install
```

### Windows

| 工具 | 下载地址 | 说明 |
|------|---------|------|
| CMake 3.16+ | https://cmake.org/download | 构建系统 |
| MinGW-w64 | https://www.mingw-w64.org | C++ 编译器（或用 MSVC） |
| Inno Setup 6 | https://jrsoftware.org/isdl.php | 安装包制作，必须 |
| ImageMagick | https://imagemagick.org/script/download.php#windows | PNG→ICO 转换，可选 |

> ImageMagick 安装时勾选 **"Add application directory to your system path"**，否则脚本找不到 `magick` 命令。
>
> 如果不安装 ImageMagick，可用在线工具 [convertio.co](https://convertio.co/png-ico/) 将 `source\pic.png` 转为 `source\pic.ico`，脚本检测到 ico 存在后会跳过转换。

首次运行需开启脚本执行权限：

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

### 图标制作

打包脚本优先使用已有的图标文件（macOS: `source/pic.icns`，Windows: `source/pic.ico`），不存在时从 `source/pic.png` 自动转换。

也可以手动制作：

```bash
# ICO → PNG（macOS 系统自带 sips）
sips -s format png source/pic.ico --out source/pic.png

# PNG → ICNS（macOS 系统自带 sips + iconutil）
mkdir AppIcon.iconset
for size in 16 32 64 128 256 512; do
    sips -z $size $size source/pic.png --out AppIcon.iconset/icon_${size}x${size}.png
    sips -z $((size*2)) $((size*2)) source/pic.png --out AppIcon.iconset/icon_${size}x${size}@2x.png
done
iconutil -c icns AppIcon.iconset -o source/pic.icns
rm -rf AppIcon.iconset

# PNG → ICO（需要 ImageMagick，或用在线工具 convertio.co【https://webfem.com/tools/ico/】）
magick convert source/pic.png -define icon:auto-resize="256,128,64,48,32,16" source/pic.ico
```

> 源图建议使用 512x512 或 1024x1024 的正方形 PNG，非正方形会导致图标拉伸变形。

## 打包安装包

### macOS（生成 .dmg）

```bash
./scripts/package_macos.sh
# 输出：dist/FluxPlayer-<版本号>.dmg
```

脚本流程：cmake Release 构建 → 生成 `.app` bundle（含 FFmpeg dylib、shaders、fonts）→ 图标处理（优先用 `source/pic.icns`，否则从 `source/pic.png` 转换）→ 打包为 `.dmg`。

### Windows（生成安装程序 .exe）

```powershell
.\scripts\package_windows.ps1
# 输出：dist\FluxPlayer-<版本号>-Setup.exe
```

脚本流程：图标处理（优先用 `source\pic.ico`，否则用 ImageMagick 从 `source\pic.png` 转换）→ cmake Release 构建 → Inno Setup 打包（含桌面快捷方式选项）。

如需指定 Inno Setup 路径：

```powershell
.\scripts\package_windows.ps1 -InnoSetup "D:\InnoSetup6\ISCC.exe"
```

## 使用方法

### 启动方式

- **无参数启动**：进入 HomeScreen 主界面，提供三种入口：
  - `OPEN LOCAL FILE`：弹出文件选择对话框打开本地视频 / 音频
  - 直接把文件拖放到窗口（提示 *or drag & drop a file here*）
  - `NETWORK URL` 输入框：粘贴网络流地址或网页视频地址后回车 / 点 `OPEN URL`
  - `MERGE VIDEOS`：进入多视频合并界面（见下文）
  - 右侧 `WATCH HISTORY` 侧栏：列出最近观看（最多 10 条），点击任意一条直接重播；每条右侧 `x` 删除单条，底部 `CLEAR ALL` 一键清空（带二次确认）
- **带参数启动**：`FluxPlayer <文件路径或URL>` 直接播放，跳过主界面

### 本地文件与纯音频

- 支持的容器 / 编解码见下方「支持格式」。打开后自动建立解码 → 队列 → 渲染 / 音频流水线。
- 检测到媒体无视频流时自动进入**纯音频模式**：展示内嵌封面（ID3 / M4A ATTACHED_PIC），无封面时显示默认图片。

### 网页视频播放

在 URL 输入框中直接粘贴 B站、YouTube 等网页地址即可播放：

```
https://www.bilibili.com/video/BV1xx...
https://www.youtube.com/watch?v=...
```

**依赖：** 需要 `yt-dlp`（已内置在 `third_party/yt-dlp/` 中，无需手动安装）

**Cookie 支持：** 播放需要登录的网页视频时，程序在主界面输入 URL 后弹出登录询问，
点击「登录并继续」会打开内置浏览器登录窗口（macOS：WKWebView，Windows：WebView2），
登录完成后 Cookie 由 FluxPlayer 自动写入 `cookies/web_cookies.txt`，下次播放同站点
可直接「使用已保存登录」。Cookie 文件路径不暴露给用户，无需手动配置。

播放网页视频时，工具栏会出现：
- **画质按钮**：切换 360P / 480P / 720P / 1080P，切换后自动 seek 到原位置
- **Download 按钮**：下载当前视频，弹出目录选择后后台下载；下载中显示进度条（百分比/速度/文件大小/ETA），支持暂停/恢复/取消，取消时自动清理未完成的临时文件

### 多视频合并与片段截取

主界面点击 `MERGE VIDEOS` 进入独立的合成界面，流程如下：

1. **添加片段**：点 `+ ADD` 多选文件，或直接拖放视频到窗口；每个文件作为一个片段加入左侧列表，同一文件可多次添加各取一段。
2. **调序 / 删除**：拖拽列表项调整合并顺序，行首 `X` 删除单项，`CLEAR` 清空。
3. **截取范围**：选中片段后，右栏用 `IN` / `OUT` 滑块设定入点 / 出点，旁边实时预览对应帧画面（拖动有 0.1s 防抖，松手立即刷新）；不设范围则使用整段（列表显示 `[full]`）。
4. **开始合并**：底部 `START MERGE`（至少需 2 个片段）。智能选择策略：
   - 全部整段且参数一致 → **流拷贝**，极速无损
   - 含截取或参数不一致 → 统一转码 **H.264 / AAC**，帧级精确对齐
5. **输出**：保存到录制目录，文件名形如 `FluxPlayer_Merge_<时间戳>.mp4`；合并完成后可 `MERGE AGAIN` 复用界面，或 `BACK` 返回主界面。

> 部分片段无音频时，输出为纯视频并给出提示。

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
| ▶ / ⏸ | 播放 / 暂停 |
| ⏹ | 停止播放 |
| `Rec V` / `Stop V` | 开始 / 停止录像（录制中按钮变红，显示时长和文件大小） |
| `Rec A` / `Stop A` | 开始 / 停止录音（录制中按钮变红，显示时长和文件大小） |
| 🔊 音量滑块 | 拖动调节音量 |
| ⚙ 设置 | 打开设置菜单（循环播放、字幕开关） |
| 🎬 画质 | 切换 360P / 480P / 720P / 1080P（仅网页视频） |
| ⏩ 倍速 | 切换播放速度（0.5x ~ 2.0x） |
| ⬇ Download | 下载当前视频（仅网页视频），支持暂停/恢复/取消，断网自动重连续传 |

### 设置菜单

| 选项 | 功能 |
|------|------|
| Loop Playback | 循环播放开关 |
| Subtitles | 字幕显示开关（内嵌字幕流） |

## 支持格式

### 视频编解码

| 编解码器 | 状态 | 硬件加速 | 说明 |
|---------|------|---------|------|
| H.264 / AVC | ✅ 已支持 | ✅ VideoToolbox / CUDA / D3D11VA / DXVA2 | 最广泛使用的视频编码 |
| H.265 / HEVC | ✅ 已支持 | ✅ VideoToolbox / CUDA / D3D11VA / DXVA2 | 4K/HDR 常用编码 |
| VP8 | ✅ 已支持 | ❌ 仅软解 | WebM 早期格式 |
| VP9 | ✅ 已支持 | ❌ 仅软解 | YouTube / WebM 常用 |
| AV1 | ✅ 已支持 | ❌ 仅软解 | 新一代编码，取决于 FFmpeg 编译选项 |
| MPEG-2 | ✅ 已支持 | ❌ 仅软解 | DVD / TS 格式 |
| MPEG-4 Part 2 | ✅ 已支持 | ❌ 仅软解 | 旧版 AVI 常见 |

### 音频编解码

| 编解码器 | 状态 | 说明 |
|---------|------|------|
| AAC | ✅ 已支持 | MP4/M4A 默认音频编码 |
| MP3 | ✅ 已支持 | 最广泛的有损音频格式 |
| FLAC | ✅ 已支持 | 无损音频格式 |
| Opus | ✅ 已支持 | WebM / 网络语音 |
| Vorbis (OGG) | ✅ 已支持 | 开源有损格式 |
| PCM / WAV | ✅ 已支持 | 无压缩原始音频 |
| AC3 / E-AC3 | ✅ 已支持 | 影视环绕声，取决于 FFmpeg 编译选项 |
| DTS | ⚠️ 部分支持 | 需 FFmpeg 编译时包含 libdca，常规构建不含 |
| WMA | ✅ 已支持 | Windows Media Audio |

### 纯音频文件播放

| 格式 | 状态 | 说明 |
|------|------|------|
| MP3 (.mp3) | ✅ 已支持 | 自动提取 ID3 内嵌封面展示 |
| FLAC (.flac) | ✅ 已支持 | 无损音频，支持封面展示 |
| WAV (.wav) | ✅ 已支持 | PCM 原始音频 |
| AAC (.aac / .m4a) | ✅ 已支持 | M4A 容器封面展示 |
| OGG (.ogg) | ✅ 已支持 | Vorbis / Opus 音频 |
| WMA (.wma) | ✅ 已支持 | Windows Media Audio |
| AIFF (.aiff) | ✅ 已支持 | Apple 无损格式 |
| APE (.ape) | ⚠️ 部分支持 | 需 FFmpeg 编译时包含相关解码器 |

> 纯音频模式：检测到无视频流时自动进入，显示内嵌封面图（ID3 ATTACHED_PIC），无封面时显示默认图片。

### 容器 / 封装格式

| 格式 | 扩展名 | 状态 | 说明 |
|------|--------|------|------|
| MP4 | .mp4 / .m4v | ✅ 已支持 | 最通用的视频容器 |
| MKV | .mkv / .mka | ✅ 已支持 | 支持多轨道、字幕 |
| AVI | .avi | ✅ 已支持 | 经典 Windows 格式 |
| MOV | .mov | ✅ 已支持 | Apple QuickTime |
| FLV | .flv | ✅ 已支持 | Flash Video / RTMP 录制 |
| WebM | .webm | ✅ 已支持 | VP8/VP9 + Opus/Vorbis |
| MPEG-TS | .ts / .mts | ✅ 已支持 | 广播传输流 |
| 3GP | .3gp | ✅ 已支持 | 移动端视频 |
| WMV | .wmv | ✅ 已支持 | Windows Media Video |

### 网络流协议

| 协议 | 状态 | 说明 |
|------|------|------|
| RTSP | ✅ 已支持 | 实时流，TCP 传输，5 秒连接超时，1MB 接收缓冲，256KB 探测 / 500ms 分析快速起播 |
| RTMP | ✅ 已支持 | 直播模式（rtmp_live=live），禁用 seek |
| HTTP / HTTPS | ✅ 已支持 | 直链点播，断流自动重连，512KB 探测 / 2s 分析 |
| HLS (m3u8) | ✅ 已支持 | HTTP Live Streaming，支持画质选择、断流重连 |
| DASH (mpd) | ✅ 已支持 | 通过 DashMerger 合并视频/音频分离流，支持 seek |
| RTP | ⚠️ 基础支持 | 可识别协议头，无专用优化参数 |
| SRT | ❌ 未支持 | Secure Reliable Transport，待开发 |
| WebRTC | ❌ 未支持 | 实时通信协议，待开发 |

### 字幕

| 格式 | 状态 | 说明 |
|------|------|------|
| SRT (SubRip) | ✅ 已支持 | 内嵌字幕流 |
| ASS / SSA | ✅ 已支持 | 内嵌字幕流，自动清理控制标签 |
| WebVTT | ✅ 已支持 | 内嵌字幕流 |
| mov_text | ✅ 已支持 | MP4 内嵌字幕 |
| 外挂字幕文件 | ❌ 未支持 | 待开发：加载 .srt / .ass 外部文件 |
| 多字幕轨切换 | ❌ 未支持 | 当前仅解码第一条字幕流，多轨选择待开发 |


## 配置文件

程序首次运行时自动在平台标准缓存目录生成 `fluxplayer.ini`，后续修改值即可，切换界面时自动重载。

| 平台 | 配置文件路径 |
|------|------------|
| macOS | `~/Library/Caches/FluxPlayer/fluxplayer.ini` |
| Windows | `%LOCALAPPDATA%\FluxPlayer\fluxplayer.ini` |
| Linux | `~/.cache/FluxPlayer/fluxplayer.ini` |

```ini
# FluxPlayer Configuration
# 配置文件自动生成在平台标准缓存目录：
#   macOS:   ~/Library/Caches/FluxPlayer/fluxplayer.ini
#   Windows: %LOCALAPPDATA%\FluxPlayer\fluxplayer.ini
#   Linux:   ~/.cache/FluxPlayer/fluxplayer.ini
# 修改后切换界面时自动重载生效。

[Audio]
# volume: 音量 (0.0 ~ 1.0)
volume=0.6

[Log]
# logLevel: 日志级别 (DEBUG / INFO / WARN / ERROR)
logLevel=INFO
# tcpLogPort: TCP 远程日志端口 (用 nc ip port 查看实时日志)
tcpLogPort=9999
# logFileEnabled: 是否将日志写入文件 (true / false)
# 默认：false
logFileEnabled=false
# logFilePath: 日志文件路径（留空则使用默认路径）
#   macOS:   ~/Library/Caches/FluxPlayer/fluxplayer.log
#   Windows: %LOCALAPPDATA%\FluxPlayer\fluxplayer.log
#   Linux:   ~/.cache/FluxPlayer/fluxplayer.log
logFilePath=

[Window]
# windowWidth: 窗口默认宽度 (像素)
windowWidth=960
# windowHeight: 窗口默认高度 (像素)
windowHeight=600

[UI]
# uiVisible: 是否显示控制面板 (true / false)
uiVisible=true
# showMediaInfo: 是否显示媒体信息面板 (true / false)
showMediaInfo=true
# showStats: 是否显示统计信息面板 (true / false)
showStats=true
# 说明：当前激活皮肤 ID（皮肤包目录名）。无效时回退到内置 cyberpunk-neon。
# 取值：cyberpunk-neon 等已安装皮肤 id
# 默认：cyberpunk-neon
skinId=cyberpunk-neon
# 说明：是否监听激活皮肤目录变更并热加载（无需重启即可预览皮肤修改）
# 取值：true / false
# 默认：true
skinHotReload=true

[Playback]
# loopPlayback: 是否循环播放 (true / false)
loopPlayback=false

[Speed]
# 说明：默认播放速度倍率
# 取值：0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0
# 默认：1.0
playbackSpeed=1.0
# 说明：慢放时是否启用帧插值（当前版本预留，暂未集成到渲染路径）
# 取值：true / false
# 默认：true
frameInterpolation=true

[Screenshot]
# screenshotDir: 截图保存目录（默认为平台缓存目录下的 Screenshot 子目录）
#   macOS:   ~/Library/Caches/FluxPlayer/Screenshot
#   Windows: %LOCALAPPDATA%\FluxPlayer\Screenshot
#   Linux:   ~/.cache/FluxPlayer/Screenshot
screenshotDir=Screenshot
# screenshotFormat: 截图格式
# 取值：png (无损压缩) | jpg (有损高质量) | yuv (I420 原始数据) | nv12 (NV12 原始数据)
# 说明：yuv/nv12 为原始像素数据，无编码开销，额外生成同名 .txt 元数据文件
screenshotFormat=png

[Record]
# recordDir: 录制文件保存目录（默认为平台缓存目录下的 Record 子目录）
#   多视频合并（MERGE VIDEOS）的输出文件也保存在此目录，文件名形如 FluxPlayer_Merge_<时间戳>.mkv/.mp4
#   macOS:   ~/Library/Caches/FluxPlayer/Record
#   Windows: %LOCALAPPDATA%\FluxPlayer\Record
#   Linux:   ~/.cache/FluxPlayer/Record
# 说明：录制为纯转封装（remux），无损保留原始流，不重编码，因此无质量档位配置。
recordDir=Record

[Decoder]
# hwaccel: 是否启用硬件加速解码 (true / false)
# macOS: VideoToolbox | Windows: CUDA(NVDEC) > D3D11VA > DXVA2
# 硬件解码可显著降低 CPU 占用，对不支持的编解码器会自动降级为软件解码
hwaccel=true

[Subtitle]
# subtitleEnabled: 是否启用内嵌字幕流解码与渲染 (true / false)
# 修改后需重新打开媒体才会启停解码；运行时仅通过设置菜单开关影响渲染
subtitleEnabled=true
# subtitleFontScale: 字幕字体缩放比例 (0.5 ~ 4.0)，1.0 为字体基准大小
subtitleFontScale=1.4
# subtitleFontPath: 自定义字幕字体路径 (留空则按平台自动探测系统 CJK 字体)
# 推荐字体：macOS=PingFang.ttc, Windows=msyh.ttc, Linux=NotoSansCJK-Regular.ttc
subtitleFontPath=

[Proxy]
# 说明：是否启用网络代理（用于访问需要代理的流媒体，如 YouTube）
# 取值：true / false
# 默认：true
proxyEnabled=true
# 说明：HTTP/HTTPS 代理地址，FFmpeg 打开 http/https 流时使用
# 取值：完整代理 URL
# 默认：http://127.0.0.1:7890
httpProxy=http://127.0.0.1:7890
# 说明：SOCKS5 代理地址（备用，部分协议可能使用）
# 取值：完整代理 URL
# 默认：socks5://127.0.0.1:7890
socksProxy=socks5://127.0.0.1:7890
```

## TCP 远程日志

编译时启用：

```bash
# xmake
xmake f --tcp_log=y && xmake

# CMake
cmake -S . -B build/cmake -DENABLE_TCP_LOG=ON && cmake --build build/cmake
```

使用 `nc` 查看实时日志：

```bash
nc <播放器IP> 9999
```

## 文件日志

在配置文件中开启：

```ini
logFileEnabled=true
```

日志文件默认保存在平台缓存目录下：

| 平台 | 日志文件路径 |
|------|------------|
| macOS | `~/Library/Caches/FluxPlayer/fluxplayer.log` |
| Windows | `%LOCALAPPDATA%\FluxPlayer\fluxplayer.log` |
| Linux | `~/.cache/FluxPlayer/fluxplayer.log` |

可通过 `logFilePath` 自定义路径。文件日志为纯文本格式（不含终端颜色码），以追加模式写入。

### Windows 控制台日志

Windows 版本使用 GUI 子系统，运行行为如下：

- 从 PowerShell、cmd 或 Windows Terminal 启动时，程序自动附着启动终端，日志直接显示；
- 从资源管理器双击启动时不创建控制台窗口，因此不会弹出额外黑框；
- 文件日志不依赖控制台，启用 `logFileEnabled=true` 后会同时写入日志文件。

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

- 命令队列 + 控制线程模型：UI 线程不直接修改播放管线状态，而是投递命令（Seek / Pause / Resume / Stop / SetSpeed / 录制 / 画质切换）到线程安全的 CommandQueue；控制线程（主循环）在每帧固定时点（UI 渲染前）串行执行，消除「UI 线程伸手进播放管线」的并发脆弱性。只读查询（getCurrentTime / isRecording 等）走原子量直读，不入队列
- 组件化拆分：Player 降为门面（Facade），持有职责单一的组件 —— DemuxWorker（读包分发 + seek 协议 + DASH 重启）、DecodeWorker ×2（video/audio 对称解码）、QueueManager（packet×2 + frame×2 队列生命周期）、ClockController（AVSync + seek 精确跳转状态）、StateManager（状态机）、RecordingService（无锁录制）、CommandQueue（命令队列）
- 多线程数据流（ffplay 式解耦）：DemuxWorker 读包线程 + 独立 video/audio DecodeWorker 解码线程 + 主渲染线程 + 平台音频回调线程，通过线程安全的 PacketQueue / FrameQueue 通信，彻底消除「视频帧队列满饿死音频」的结构性死锁
- 流水线式处理：Demuxer → PacketQueue → Decoder → FrameQueue → Renderer / AudioOutput
- 状态机管理：StateManager 通过 `PlayerState` 枚举管理 IDLE → OPENING → PLAYING → PAUSED → STOPPED 状态转换，统一转换协议与回调
- 录制无锁化：RecordingService 的录制器创建/销毁/writePacket 全归 demux 线程串行执行，查询走原子状态块，彻底去除录制锁

### 视频渲染

- 双格式纹理支持：YUV420P（Y/U/V 三纹理）和 NV12（Y + UV 双纹理）
- GLSL 片段着色器通过 `isNV12` uniform 切换 YUV420P/NV12 采样路径
- 色彩空间自适应：从 AVFrame 元数据提取 `colorspace`/`color_range`，自动选择 BT.601/BT.709/BT.2020 转换矩阵和 TV/PC 量化范围；元数据缺失时按分辨率启发式选择（≥720p → BT.709）
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
- 主时钟自适应选择：有音频时以音频时钟为主（音频回调按真实采样率推进，视频追随，最稳）；无音频或音频输出失败时退回外部时钟（墙钟）
- 音频主时钟在回调间隙基于 steady_clock 实时插值，消除按音频缓冲周期（~25ms）阶梯跳变导致的画面卡顿；插值上限 0.1s 防止音频线程卡死时时钟外推跑飞
- VSync 驱动每帧取一帧渲染，无效 PTS 帧基于帧间隔估算补偿
- 音频帧部分消费残留缓冲，避免数据丢失导致播放速度异常

### 网络流处理

- 支持 RTSP / RTMP / HTTP / HLS 协议
- 实时流识别：URL 协议头检测 + HLS 格式名 + duration==0 多重判断，修复 RTMP 被解析为 FLV 格式名漏判
- 按协议设置专用选项：HLS 断流重连、RTSP 1MB 缓冲 + 256KB 探测 / 500ms 分析、RTMP 直播模式
- 实时流 PTS 基准校准（音视频首帧 PTS 对齐）
- PTS 回绕检测：视频回绕时跳帧等待，音频回绕时统一重校准基准
- PTS 异常检测：归一化后倒退 > 0.5s 或前跳 > 30s 用估算值代替；入队前强制单调递增防进度条抖动
- 无效 PTS（AV_NOPTS_VALUE）帧基于 best_effort_timestamp 兜底取值，仍无效则按帧间隔估算
- 实时流起播追赶：丢弃首个关键帧之前的视频包；prebuffer 期间收到新 IDR 则重置队列从最新关键帧起播
- 网络断流指数退避重试（100ms → 3000ms，最多 30 次）
- 实时流视频队列 3 帧 + 预缓冲 2 帧低延迟起播，音频队列 8 帧；点播流队列加深应对抖动；背压机制防止欠载

### FFmpeg 版本兼容

- 通过 `LIBAVCODEC_VERSION_MAJOR` 宏自动适配 FFmpeg 4.x（channels）和 5.x+（ch_layout）API 差异
- `swr_alloc_set_opts` / `swr_alloc_set_opts2` 自动选择

