# ImGui 落地路线

当前 UI 主要在：

- `src/ui/HomeScreen.cpp`
- `src/ui/Controller.cpp`
- `include/FluxPlayer/ui/HomeScreen.h`
- `include/FluxPlayer/ui/Controller.h`

## 1. 先建立主题常量

建议在 `src/ui` 下新增 `CyberpunkTheme.h/.cpp`，或先以 `Controller.cpp` / `HomeScreen.cpp` 的 `static` helper 形式落地。

核心内容：

```cpp
namespace FluxPlayer::UI {
struct CyberColor {
    static constexpr ImU32 VoidBg      = IM_COL32(3, 5, 17, 255);
    static constexpr ImU32 PanelBg     = IM_COL32(7, 13, 34, 225);
    static constexpr ImU32 Cyan        = IM_COL32(0, 232, 255, 255);
    static constexpr ImU32 CyanSoft    = IM_COL32(45, 167, 255, 255);
    static constexpr ImU32 Violet      = IM_COL32(168, 85, 255, 255);
    static constexpr ImU32 Magenta     = IM_COL32(255, 61, 242, 255);
    static constexpr ImU32 Rose        = IM_COL32(255, 59, 122, 255);
    static constexpr ImU32 Text        = IM_COL32(234, 248, 255, 255);
    static constexpr ImU32 MutedText   = IM_COL32(111, 137, 168, 255);
};
}
```

## 2. 首页改造

目标文件：`src/ui/HomeScreen.cpp`

改动点：

1. `setupStyle()`
   - 统一降低圆角到 3-6。
   - Frame/Button 背景透明或深蓝。
   - Text 使用 ice white，弱文本使用 steel blue。

2. `renderBackground()`
   - 绘制暗色透视网格、扫描纹理、横向数据轨以及顶部/底部能量条。
   - 在中央加入低透明度的 cyan/violet 空间辉光。

3. `renderUI()`
   - 标题使用双色轻微错位发光；中央绘制无交互的六边形/环形能量核心。
   - 使用 `DrawCyberFrame` 绘制左右两块来源卡片：`LOCAL FILE` 与 `NETWORK URL`。
   - 页面只保留打开本地文件和 URL 输入/提交两种交互入口。
   - 打开文件按钮内绘制文件夹图标，URL 输入区改成 violet 数据插槽并采用 `OPEN URL` 文案。

## 3. URL 打开中状态

当前缺口不是颜色问题：`main.cpp` 会在 `HomeScreen::run()` 返回后销毁主页，再同步调用 `playMedia()`；而 `Player::open()` 中的 `EXTRACTING`、`OPENING` 和 `initWindowAndRenderer()` 都发生在 `Controller` 创建之前。URL 提取或媒体连接耗时时，现有架构没有任何窗口可以反馈状态。

目标稿：`source/UI/mockup_opening.svg`。

建议落地方向：

1. URL 提交后保留一个可渲染窗口/ImGui 循环，立即显示播放器外壳和 opening overlay。
2. 将耗时的 `Player::open()` 调度到工作线程，或新增独立 `OpeningScreen` 承载异步打开结果；主线程继续 poll/render，不能在 UI 线程同步等待提取。
3. 将状态事件映射为步骤反馈：`EXTRACTING` -> `RESOLVE SOURCE`，`OPENING` -> `OPEN MEDIA STREAM`，启动播放后至首帧/预缓冲完成 -> `BUFFER FIRST FRAME`。
4. 当前首帧和 `prebuffering_` 信息没有直接提供给 UI 的公共回调；落地时应增加只读 opening 状态/首帧 ready 通知，而不是凭计时器隐藏面板。
5. 打开失败时保持同一窗口展示错误及返回首页动作；成功时在首帧显示后平滑去除 overlay，启用底部 Dock。

验收要求：

- 从点击 URL `OPEN` 开始到首帧或错误反馈出现，始终存在可见窗口和状态文字。
- 长时间网页提取、DASH 初始化、demuxer 网络打开及直播预缓冲均有对应阶段。
- 状态轨不展示伪造百分比；可显示活动扫描线与已等待时间。

## 4. 播放控制层改造

目标文件：`src/ui/Controller.cpp`

### 4.1 `Controller::init()`

将当前散落的 ImGui style 调整为全局 CyberpunkTheme：

- `WindowRounding = 4`
- `FrameRounding = 3`
- `PopupRounding = 4`
- `WindowBorderSize = 0`
- `FrameBorderSize = 1`
- `GrabRounding = 2`

### 4.2 `renderBottomOverlay()`

建议将 `overlayH` 从 `64.0f` 调到 `82.0f`。

结构变化：

- DrawList 先绘制 Dock 背景、切角边框、顶部渐变光带、两侧刻线。
- ImGui window 背景设透明，避免双重背景。
- 第一行：进度条 + 时间码。
- 第二行：左下载状态、中播放按钮、右工具按钮。

### 4.3 `renderProgressBar()`

保留现有点击/拖动逻辑，只替换绘制：

- 热区仍用 Invisible/Button。
- 可视轨道高度从 16 视觉上压到 10，热区仍 18。
- Hover tooltip 增加 cyan 顶边。
- 播放头增加 violet 外圈，不要只用 cyan 圆点。

### 4.4 `renderPlaybackButtons()`

当前已经用 DrawList 画 play/pause/stop，继续扩展：

- 增加统一按钮框 helper。
- 录制按钮加入 rose pulse。
- 中央播放按钮比其他按钮更宽，形成视觉主锚点。

### 4.5 `renderVolumeAndSettings()`

改造方向：

- 设置、音量、速度、画质全部用统一 `DrawNeonIconButton`。
- 音量 slider 展开时绘制为小型机械滑轨。
- 窄屏时画质/速度可以隐藏到设置菜单。
- 设置按钮必须是可辨识的齿轮或 `SET`；菜单保留循环播放与字幕，并补充媒体信息、统计的显示开关及截图、全屏、显隐、退出快捷键提示。

### 4.6 Popup 与 HUD

涉及函数：

- `renderSpeedButton()`
- `renderQualityButton()`
- 设置菜单区
- `renderMediaInfo()`
- `renderStats()`

统一为：

- 深蓝黑半透明背景。
- 顶部标题栏。
- 切角边框。
- hover 项左侧光条。

### 4.7 `renderDownloadButton()` / `renderDownloadProgress()` / `renderDownloadInfo()`

- 网页视频下保留下载入口；下载开始后显示百分比进度、暂停/继续与取消操作。
- `renderDownloadInfo()` 的下载速度、文件大小和 `ETA` 必须随 active download module 一同设计与实现，不能仅保留进度槽。
- 状态更新期间保留已有的过渡行为：暂停或重连时不覆盖最后一次有效进度。

## 5. 推荐实现顺序

1. 新增 theme/helper，保证首页和播放器共用颜色。
2. 改首页，仅提供本地文件与 URL 两种入口。
3. 增加 opening 可见窗口与异步打开状态桥接，先解决 URL 等待期间无反馈的问题。
4. 改底部 Dock 和进度条，这是播放页最关键体验。
5. 改按钮组、速度、画质、音量。
6. 改媒体信息、统计、设置菜单。
7. 做窄屏检查。

## 6. 验收清单

- 首页、播放页、弹窗使用同一组 cyan/violet/magenta。
- 首页只展示打开本地文件与 URL 两种入口。
- URL 从提交到首帧或报错之间，播放器 loading shell 始终可见，并正确表示解析、打开流、首帧缓冲步骤。
- 控制条自动隐藏仍正常。
- 字幕不会被底部 Dock 遮挡。
- 进度条拖动、量化跳转、tooltip 仍正常。
- 录制、下载、画质切换、速度切换、设置菜单仍可操作。
- 下载中能查看百分比、下载速度、文件大小和预计剩余时间，并可暂停/继续或取消。
- 录像 `REC V` 与录音 `REC A` 是两个独立可点击控件，并分别正确显示录制态、时间与大小。
- 设置入口可见且可辨识，菜单可访问循环播放、字幕、媒体信息和统计；`SPACE`、方向键、截图 `P`、全屏 `F`、UI 显隐 `H`、退出 `ESC` 的现有快捷入口有提示。
- 800px 宽窗口没有文本挤出按钮。
- 视频画面亮度较高时，Dock 仍可读。
- 深色视频画面时，Dock 不显得过重。

## 7. 现有能力对照表

落地前后均需逐项检查，不得因为新布局省略已有功能：

| 能力 | 当前代码入口 | 新界面落点 |
| --- | --- | --- |
| 播放/暂停、停止、Seek | `renderPlaybackButtons()` / `renderProgressBar()` | Dock 中心 / Row 1 |
| 录像、录音 | `Player::startVideoRecording()` / `startAudioRecording()` | `REC V` / `REC A` |
| 音量、静音 | `renderVolumeAndSettings()` | 右侧扬声器与滑轨 |
| 速度 | `renderSpeedButton()` | `1.0x` chip；窄屏进入 Settings |
| 网页画质、下载 | `renderQualityButton()` / `renderDownloadButton()` / `renderDownloadInfo()` | 左侧 active download module，包含进度、暂停/取消、速度、大小、ETA；窄屏画质进入 Settings |
| 循环、字幕 | 当前 SettingsMenu | `SETTINGS` HUD toggle |
| 媒体信息、统计 | `renderMediaInfo()` / `renderStats()`，快捷键 `I` / `S` | Settings toggle + 右侧 HUD |
| 键盘控制 | `Player` 快捷键 `SPACE`、方向键、`F`、`P`、`I`、`S`、`H`、`ESC` | Settings 底部提示 |
