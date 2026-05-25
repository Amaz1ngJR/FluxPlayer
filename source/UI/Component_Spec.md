# FluxPlayer 赛博朋克组件规范

## 1. 全局 Token 摘要

详见 `ui_tokens.json`。这里列核心值：

| 名称 | Hex | 用途 |
| --- | --- | --- |
| `bg.void` | `#030511` | 全局最深背景 |
| `bg.panel` | `#070D22` | 面板底 |
| `cyan.core` | `#00E8FF` | 主操作、主描边 |
| `cyan.soft` | `#2DA7FF` | 辅助蓝光 |
| `violet.core` | `#A855FF` | 副强调、右侧光 |
| `magenta.core` | `#FF3DF2` | 强强调、播放头尾光 |
| `rose.alert` | `#FF3B7A` | 录制、错误、危险 |
| `text.primary` | `#EAF8FF` | 主文字 |
| `text.muted` | `#6F89A8` | 弱文字 |

## 2. DrawList Helper

建议新增一个内部 helper 文件或放在 `Controller.cpp` / `HomeScreen.cpp` 静态函数区。

### 2.1 `DrawCyberFrame`

用途：绘制首页主面板、底部 Dock、右侧 HUD 面板。

参数建议：

```cpp
void DrawCyberFrame(
    ImDrawList* dl,
    ImVec2 min,
    ImVec2 max,
    ImU32 border,
    ImU32 glow,
    float rounding = 4.0f,
    float cut = 14.0f,
    bool active = false);
```

绘制顺序：

1. 半透明面板底。
2. 外发光 3-5 层。
3. 主边框。
4. 四角 L 形切角线。
5. 顶部或左侧 2px 能量条。

### 2.2 `DrawCircuitTicks`

用途：在 Dock 两端、面板标题栏、进度轨道下方绘制短刻线。

```cpp
void DrawCircuitTicks(ImDrawList* dl, ImVec2 origin, int count, float gap, bool vertical, ImU32 color);
```

规则：

- 每 4 条刻线加长一次。
- alpha 不超过 90。
- 不参与交互，仅做结构装饰。

### 2.3 `DrawNeonIconButton`

用途：统一播放、停止、设置、音量、画质、速度、下载按钮。

尺寸：

- 主播放按钮：`84 x 30`
- 常规图标按钮：`30 x 30`
- 文本 chip：`58-70 x 30`
- 录制按钮：`64-78 x 30`

状态色：

- normal：透明底 + 55% cyan 边。
- hover：10% cyan 填充 + 90% cyan 边 + 外发光。
- active：16% violet/cyan 填充。
- disabled：文字与边框 alpha 降至 35%。

## 3. 首页组件

### 3.1 启动控制台

- 标题区：顶部居中，标题 `FLUX // PLAYER` 使用 cyan/violet 错位微光。
- 中央核心：直径约 `244`，六边形与环形轨道组成纯装饰视觉，不响应点击。
- 来源卡片：左右并列，每块约 `570 x 200`，底部居中排列，间距 `96`。
- 左卡主边为 cyan，右卡主边为 violet/magenta，均使用切角双层机械框。
- 可交互内容仅包含本地文件按钮与 URL 输入/提交区，不显示拖放、最近项目或核心点击入口。

### 3.2 打开文件按钮

- 宽：`472`
- 高：`48`
- 文案：`OPEN LOCAL FILE`
- 左侧绘制文件夹线框图标。
- Hover 时边框与按钮内沿增强 cyan 辉光。

### 3.3 URL 插槽

- 输入框宽：`332`
- 输入框高度：`48`
- 背景：`#030817`
- 边框：violet 55%
- 激活：magenta 85%，底部加 1px 扫描线。
- `OPEN URL` 按钮宽 `140`，使用完整动作文案。

## 4. Opening 状态组件

用于 URL 提交后到首帧显示前的持续反馈窗口，参见 `mockup_opening.svg`。

### 4.1 播放器外壳

- 整个窗口从提交当帧开始可见，背景使用接近视频黑位的 `#030511`。
- 顶部状态轨显示来源类型与 `OPENING STREAM` 芯片。
- 底部 Dock 与正常播放页同位置出现，但 seek 和 transport 在首帧前使用 disabled 状态。

### 4.2 中央状态面板

- 宽：`560`；推荐高：`360-400`；始终居中。
- 标题：`OPENING STREAM`；正文：`Preparing network playback. This may take a moment.`
- 按顺序展示 `RESOLVE SOURCE`、`OPEN MEDIA STREAM`、`BUFFER FIRST FRAME` 三个步骤，每项只有 `DONE` / `ACTIVE` / `WAITING`。
- 显示已等待时长与截断后的 URL，证明当前请求没有丢失。
- 轨道默认是循环扫描活动指示，不显示虚构的下载/连接百分比；若未来暴露真实预缓冲深度，再显示实际值。

### 4.3 状态切换

| 内部状态 | 中央面板行为 | Dock 行为 |
| --- | --- | --- |
| `EXTRACTING` | 解析来源为 active | 禁用 |
| `OPENING` | 媒体流为 active | 禁用 |
| 首帧/预缓冲等待 | 缓冲首帧为 active | 禁用 |
| 首帧可见 | 面板淡出 | 切换为正常播放 Dock |
| `ERRORED` | 显示错误摘要与返回入口 | 隐藏或禁用 |

## 5. 播放页组件

### 5.1 Bottom Dock

- 高度：`82`
- Padding：左右 `14`，上下 `8`
- 背景：`#030715` alpha 0.78
- 顶部光带：2px，cyan -> violet -> magenta
- 内部分两行：
  - Row 1：进度条 `18px` 高，时间码右对齐
  - Row 2：按钮区 `30px` 高

响应式：

- 宽度 < 900：隐藏 `REC V/REC A` 的完整文字，只保留 `V` / `A` 图标态。
- 宽度 < 720：画质、速度合并到设置菜单。

### 5.2 进度轨

- 交互热区高：`18`
- 可视轨道高：`10`
- 背景：`#101936`
- 边框：cyan 35%
- 已播放：cyan -> violet -> magenta
- 播放头：半径 `5`，外发光半径 `13`
- Hover tooltip：深色小面板，cyan 上边线。

### 5.3 音量滑轨

- 默认只显示音量图标。
- Hover 展开宽 `112`。
- 滑轨高度 `8`，已填充使用 cyan。
- 静音时图标与轨道变 muted blue，音量头变 violet。

### 5.4 速度/画质 Chip

- 高：`30`
- 宽：`60-72`
- 字体：等宽 12px
- 速度使用 cyan 边，画质使用 violet 边。
- 当前选项在 popup 内用左侧 2px 光条标识。

### 5.5 录制状态

- 未录制：低亮 cyan 边框。
- 录制中：rose 边框 + rose 小圆点脉冲。
- 时间与大小显示使用 `ShareTechMono`，颜色为 `#FF6A9D`。
- 录像与录音是两个独立按钮：`REC V` 调用视频录制，`REC A` 调用音频录制；任何宽度下都不能合并成单一 `REC`。
- 宽度不足时可将文字压缩为 `V` / `A`，但按钮仍必须分别可点击且具有录制态反馈。

### 5.6 下载状态

- 未下载时：显示 `DOWNLOAD` 入口，仅网页视频可用；不使用 `DL` 缩写。
- 下载中：入口右侧依次展示进度槽与百分比、暂停/继续按钮、取消按钮。
- 状态文字为双行紧凑布局：第一行显示下载速度与文件大小（例如 `8.6 MiB/s  124 MiB`），第二行显示预计剩余时间（例如 `ETA 00:18`）。
- 暂停或断线重连期间保留最近有效进度；状态文字可显示暂停/重连提示，不应令进度条跳回零。
- 下载状态为现有功能的一部分，在窄屏方案中也不可完全隐藏；空间不足时可将速度/ETA 移至下载 HUD 面板。

## 6. 右侧 HUD 面板

统一规格：

- 宽：`340`
- 距右：`18`
- 距上：`64`
- 背景：`#050A1A` alpha 0.88
- 标题高：`34`
- 行高：`24`
- 键列宽：`110`

信息层级：

- 标题：cyan，全大写。
- 分区线：cyan 到透明渐变。
- 键：muted blue。
- 值：ice white。
- 重要值：cyan。
- 警告值：rose。

## 7. Popup 菜单

用于设置、速度、画质。

- 背景：`#050A1A` alpha 0.96。
- 边框：cyan/violet 根据入口类型选择。
- 每项高 `28`。
- Hover：左侧出现 2px cyan 光条，背景加 10% cyan。
- CheckMark：不要用默认勾，建议用小型发光菱形或短横。

### 7.1 设置菜单内容

设置入口在 Dock 右侧明确显示为齿轮或 `SET`，不可仅用无法辨识的装饰符号代替。至少覆盖：

| 设置项 / 入口 | 类型 | 当前功能来源 |
| --- | --- | --- |
| Loop Playback | toggle | `Player::setLoopPlayback()` |
| Subtitles | toggle | `Controller::setSubtitleEnabled()` |
| Media Info | toggle | `Controller::toggleMediaInfo()` / `I` |
| Statistics | toggle | `Controller::toggleStats()` / `S` |
| Record Quality | status/selector | `Config::recordQuality` |
| Screenshot | 快捷键提示 | `P` |
| Fullscreen / Exit | 快捷键提示 | `F` / `ESC` |
| HUD Visible | 快捷键提示 | `H` |

提示区同时保留 `SPACE` 播放/暂停和方向键跳转提示。窄屏时，速度和网页画质选择可以移动到该菜单内，但不得移除功能。

## 8. 字幕

字幕不要直接套蓝紫风，否则影响观看。

推荐：

- 主字：白色。
- 描边：黑色 75%。
- 阴影：黑色 55%。
- 字幕背景：默认无；必要时使用 35% 黑色柔和底。
- UI Dock 显示时，字幕自动上移，避免与 Dock 重叠。
