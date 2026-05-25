# FluxPlayer Cyberpunk UI Design

本目录是 FluxPlayer 的蓝紫赛博朋克 UI 设计包，基于 `source/pic.png` 的图标风格整理。

## 文件

- `FluxPlayer_Cyberpunk_UI_Design.md`：完整 UI 方案，从视觉定位、首页、播放页、面板到交互动效。
- `Component_Spec.md`：组件级规范，包含尺寸、状态、颜色、ImGui 绘制建议。
- `ImGui_Implementation_Map.md`：面向当前 C++/Dear ImGui 代码的落地路线。
- `ui_tokens.json`：颜色、间距、圆角、透明度、动效等设计 token。
- `mockup_home.svg`：精简启动页，只暴露打开本地文件与 URL 两种入口。
- `mockup_opening.svg`：URL 解析、连接与首帧缓冲期间始终可见的播放器过渡状态。
- `mockup_player.svg`：播放页控制层视觉稿。
- `mockup_components.svg`：核心控件与状态视觉稿。

## 功能完整性

设计稿不只是换肤：`mockup_player.svg` 与组件规范必须保留当前播放器已有入口，包括播放/暂停、停止、进度跳转、音量/静音、速度、网页画质，以及下载进度、暂停/取消、速度、文件大小和预计剩余时间；同时保留录像 `REC V`、录音 `REC A`、设置、媒体信息、统计、字幕、循环播放，以及截图、全屏、UI 显隐和退出等快捷键提示。

URL 提交后不得出现空窗、白屏或无反馈等待：应用应立即进入 `mockup_opening.svg` 表示的持久在线状态，直到首帧可显示或明确失败。

当前实现映射以 `src/ui/Controller.cpp` 和 `src/core/Player.cpp` 为准；若新增能力，设计稿和验收清单须同步更新。

## 设计方向

关键词：深黑金属、青蓝主光、紫/品红副光、机械切角、发光轨道、细密电路刻线、HUD 信息层。

核心原则：视频内容永远优先，UI 像一套轻量机甲外壳贴在画面边缘，而不是大面积遮挡画面。
