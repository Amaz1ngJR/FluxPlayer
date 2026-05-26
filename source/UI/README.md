# FluxPlayer 皮肤包规范

本目录是 FluxPlayer 皮肤系统的**协议层**：

```
source/UI/
├── README.md            ← 你正在看的文档（皮肤作者规范）
├── skin.schema.json     ← 单一权威：字段、类型、取值范围
├── skins/
│   └── <id>/
│       ├── skin.json    ← 皮肤清单（必备）
│       ├── preview.svg  ← 预览图（推荐）
│       ├── mockup_*.svg ← 当前皮肤状态稿（可选，文档用）
│       └── 其他资源     ← 字体、纹理（可选）
└── concepts/
    └── <concept-id>/    ← 尚未实现为皮肤的未来视觉稿，不参与加载/打包
```

> **唯一权威是 `skin.schema.json`**。本 README 解释「字段语义」「与 ImGui 全局样式的关系」「常见陷阱」，但**字段名、必填项、取值范围以 schema 为准**。
> `concepts/future-neon-console/` 保存的是未来设计探索；不要将其中 SVG 当作 `cyberpunk-neon` 当前默认实现的验收稿。

---

## 1. 谁应该读这份文档

- 想修改 `cyberpunk-neon` 或新增皮肤的设计师 / 工程师
- 修改 `SkinRenderer::ApplyImGuiStyle` 或新增 token 消费点的开发者

如果你只想换个配色：复制 `skins/cyberpunk-neon/` 为新目录、改 `id` 与颜色即可，跳到 §6「编辑工作流」。

---

## 2. 加载顺序与回退链

`SkinManager` 按以下优先级查找皮肤目录：

| 层级 | 路径 | 用途 |
| ---- | ---- | ---- |
| User | `<AppData>/skins/<id>/` | 用户自行安装的皮肤 |
| Dev | `<cwd>/source/UI/skins/<id>/` | 仓库内开发期皮肤（本目录） |
| BuiltIn | `<exe>/resources/skins/<id>/` 或 macOS bundle `Resources/skins/<id>/` | 构建产物随附的内置皮肤 |

加载失败时按这个链路回退：

1. 请求的 `skinId` 在三层中第一个能成功**解析+校验**的版本
2. 若全部失败 → 强制加载内置 `cyberpunk-neon`
3. 若连内置都坏了 → 编译期最小默认快照（`makeBuiltInFallback`），保证 UI 仍可绘制

校验失败的具体原因写入 `SkinManager::lastError()`，会显示在 Settings → Appearance 状态行。

---

## 3. 字段语义 —— 关键区分：**全局** vs **容器局部**

> 这是本规范的核心。**搞错这两类字段的归属，会导致一处皮肤改动污染整个 UI**。
> 历史教训：早期把 `dockPaddingX/Y` 错绑到 ImGui 全局 `FramePadding`，结果 dock 容器自己的边距字段把所有按钮的文字内边距撑大、文字撑出按钮边框。

### 3.1 全局字段（写一次，整个 UI 都受影响）

这些字段在 `ApplyImGuiStyle` 中直接写入 `ImGuiStyle`，作用于**所有窗口、所有控件**。修改它们要谨慎，幅度不要过大。

| 皮肤字段 | 映射到 ImGuiStyle |
| --- | --- |
| `roles.background.panel` | `Colors[ImGuiCol_ChildBg]`、`MenuBarBg`、`TitleBg*` |
| `roles.background.panelTransparent` | `Colors[ImGuiCol_WindowBg]`、`PopupBg` |
| `roles.text.primary` | `Colors[ImGuiCol_Text]` |
| `roles.text.muted` | `Colors[ImGuiCol_TextDisabled]` |
| `roles.line.primary` | `Colors[ImGuiCol_Border]` |
| `roles.accent.primary` | `Button*`/`Header*`/`SliderGrab*`/`CheckMark` 等交互态 |
| `metrics.radius.panel` | `WindowRounding`、`ChildRounding` |
| `metrics.radius.button` | `FrameRounding`、`GrabRounding`、`TabRounding` |
| `metrics.radius.popup` | `PopupRounding` |
| `metrics.spacing.panelPadding` | `WindowPadding`（x 与 y 同值） |
| `metrics.spacing.controlGap` | `ItemSpacing.x`、`ItemInnerSpacing.x` 基准 |
| `metrics.spacing.rowGap` | `ItemSpacing.y`、`ItemInnerSpacing.y` 基准 |

**绝对不要**把以下「容器局部」字段写入全局 `ImGuiStyle`：

- ~~`surfaces.player.dockPaddingX/Y` → `FramePadding`~~ ❌（曾经的 bug）

### 3.2 容器局部字段（只在特定 Surface 内消费）

这些字段由具体的渲染器**显式 `PushStyleVar` 或直接当数值用**，**不会**落到全局 `ImGuiStyle`。

| 字段 | 谁在用 | 含义 |
| --- | --- | --- |
| `surfaces.player.dockPaddingX/Y` | `Controller::renderBottomOverlay` 的 dock window | 底部 dock 容器自身的内边距 |
| `metrics.size.bottomDockHeight` | dock window 的固定高度 | dock 是定高容器，不能被布局推开 |
| `metrics.size.progressHotHeight` / `progressVisualHeight` | 进度条交互区 / 视觉条厚度 | 进度条几何 |
| `metrics.size.mainPlayButton` / `iconButton` / `chipButton` | 各按钮的 `[w, h]` | 单个控件几何 |
| `metrics.size.homeSourceCard` / `openingPanel` | Home / Opening 卡片几何 | 单个 Surface 几何 |
| `metrics.opacity.dock` / `hudPanel` / `popup` | 对应窗口的整体透明度乘子 | 必须和具体绘制点配合 |
| `metrics.opacity.subtleDecoration` | 背景装饰（光晕、网格、扫描线）alpha 乘子 | **永远是「乘子」，不是绝对 alpha** |
| `metrics.opacity.disabled` | 禁用态控件 alpha 乘子 | 同上 |
| `motion.autoHideDelaySeconds` | 鼠标停滞后 dock 自动隐藏的秒数 | 行为时序 |
| `motion.scanlineSpeed` / `pulseSpeed` | 装饰动画速度 | 单位约定见 §4 |
| `motion.hoverGlowMs` | hover 发光淡入毫秒 | 装饰动画 |
| `motion.reloadDebounceMs` | 热加载防抖窗口（毫秒） | 影响皮肤改完到生效的延迟 |
| `typography.titlePx` / `panelTitlePx` / `bodyPx` / `timecodePx` / `buttonPx` | 各类文字字号 | **目前仅 `titlePx` 被消费**（HomeScreen 主标题发光层用）。其余字段请填合理值，未来会接入字体 atlas 重建 |
| `typography.displayFamily` / `bodyFamily` | 主标题 / 正文字体名 | 当前未触发字体 atlas 重建（阶段 5 之后会接入），可写期望值 |
| `decoration.cutCorners` | 切角装饰开关 | 关掉 → 卡片走普通圆角 |
| `decoration.glow` | 发光层开关 | 关掉 → 不画外发光（性能更好） |
| `decoration.scanlines` | 扫描线开关 | 关掉 → 静态背景 |
| `decoration.circuitTicks` | 透视地板网格 + 数字雨开关 | 关掉 → 干净背景 |

`surfaces` 是局部布局的权威区，完整覆盖当前皮肤必须展示的界面：

| 对象 | 主要控制内容 |
| --- | --- |
| `surfaces.home` | 主页卡片 padding、本地按钮、URL 行、登录弹窗、背景装饰密度 |
| `surfaces.opening` | 慢 URL 打开时面板、文案位置、点阵动画与重绘节奏 |
| `surfaces.player` | Dock 行距、播放/录制/工具/下载控件尺寸及对齐 |
| `surfaces.hud` | Media Info 与 Statistics 面板位置和大小 |
| `surfaces.settings` | 设置模态、左侧分页导航、Appearance 卡片与状态条几何 |
| `surfaces.popup` | 速度和画质菜单的行高、宽度与偏移 |
| `surfaces.subtitle` | 字幕安全区域、宽度与背景透明度 |

### 3.3 不被皮肤控制的字段（**故意**）

为了避免**单字段污染全局**，下列 ImGui 样式**故意不**绑定皮肤字段：

- `FramePadding`（按钮内文字到边框的距离）— 全局保持 ImGui 默认 `(4, 3)`。
  - 理由：FluxPlayer 多个按钮用固定高度（如 dock 内 22 px），太大的 `FramePadding` 会顶破布局；任何**容器内的局部需求**（例如设置弹窗想要更大的按钮）应在该容器内 **`PushStyleVar(FramePadding, ...)` + `PopStyleVar()`**，而不是改全局。
  - 局部控件需要特定 padding 时，应在其所属 `surfaces.*` 下新增字段，不要复用其他容器字段。
- `WindowBorderSize` / `FrameBorderSize` / `PopupBorderSize`：固定为 `1.0`。皮肤通过颜色（`roles.line.*`）和发光（`decoration.glow`）来表达边框强度，不通过粗细。

### 3.4 控件级排版铁律（避免视觉错位）

> 这一节是**给消费 token 的渲染代码作者看的**，不影响皮肤 JSON 字段。但每条都对应过去出现的 bug，违反就会复发。

#### 3.4.1 同一行的控件必须共用 `FramePadding`

ImGui 控件高度的计算是 `lineHeight + FramePadding.y * 2`。如果同一行里两个控件**用了不同的 `FramePadding.y`**，它们的视觉高度就会错位。

```cpp
// ✘ 反例：输入框高、按钮矮（曾经的 OPEN URL 按钮 bug）
ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 10));
ImGui::InputText("##url", buf, sz);
ImGui::PopStyleVar();
ImGui::SameLine();
ImGui::Button("OPEN URL");  // 用全局 FramePadding (4,3) → 比输入框矮

// ✓ 正例：同行控件共用同一个 FramePadding push 周期
ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 10));
ImGui::InputText("##url", buf, sz);
ImGui::SameLine();
ImGui::Button("OPEN URL");
ImGui::PopStyleVar();
```

#### 3.4.2 一组按钮（按钮组）应当尺寸 / 间距完全一致

- **宽度**：按内容预算的最长文案 + 余量；**或**用 `(contentW - gap*(n-1)) / n` 等分。等分时若文案差异大，宁可多余空白也别按文案宽度分配——那样视觉重心不齐。
- **高度**：永远通过 `FramePadding` 而**不是**给每个按钮传不同 `ImVec2(_, h)`。传死高度会跳过 ImGui 的自然垂直对齐。
- **间距**：用统一的 `ImGui::GetStyle().ItemSpacing.x` 或 `SameLine(0, gap)`，不要混用。

#### 3.4.3 弹窗的最小尺寸约束

- **超过 3 个交互项 / 包含 Combo / 包含按钮组 → 用居中模态窗口**，宽度至少 `min(640, displayW * 0.9)`。
- **小型菜单（≤ 3 项 checkbox）才用 dropdown**，并且要 `AlwaysAutoResize` + 文字预算合理（计算最长项的 `CalcTextSize().x + padding * 2`）。
- 历史教训：早期 `showAppearanceMenu_` 子页用 `SetNextWindowSize(280, 0)`，combo 项 `cyberpunk-neon (BUILT-IN)` + `RESTORE DEFAULT` 被裁切；改造为 `renderSettingsModal()`（`src/ui/Controller.cpp`），最大宽度由 `surfaces.settings.maxWidth` 控制、按内容自适应高度、ESC / 关闭按钮关闭。

#### 3.4.4 临时风格 push/pop 必须配对

任何 `PushStyleVar` / `PushStyleColor` 在同一函数内必须有对应数量的 `PopStyleVar` / `PopStyleColor`。**栈不平衡 ImGui 不会立刻报错**——它会等到下一帧 `ImGui::Render()` 时 assert 崩。复杂分支（多个 `if/else` 内 push）一定要跟一个 `Pop` 在公共出口。

#### 3.4.5 容器局部 padding 不能写入全局 ImGuiStyle

如 §3 反复强调：模态对话框需要更宽松的 `FramePadding`、dock 需要更紧凑的 `ItemSpacing`、Home 卡片需要 `WindowPadding(32, 28)`——这些都属于**容器局部**，必须用 `PushStyleVar` 包住自己的 `Begin/End` 块，不能直接改 `ImGui::GetStyle()`，否则会污染本帧后续所有窗口。

---

## 4. 单位与坐标系

- **像素**：`metrics.spacing.*`、`metrics.size.*`、`metrics.radius.*`、`typography.*Px`。**逻辑像素**（与 `io.DisplaySize` 同坐标系），高 DPI 下由 ImGui 后端自动放大。
- **0..1 比例**：`metrics.opacity.*`。
- **秒**：`motion.autoHideDelaySeconds`。
- **毫秒**：`motion.hoverGlowMs`、`motion.reloadDebounceMs`。
- **每秒像素**：`motion.scanlineSpeed`（扫描线纵向偏移速度）、`motion.pulseSpeed`（脉冲频率，单位 Hz 量级）。

所有数值字段的有效区间在 `skin.schema.json` 的 `minimum`/`maximum` 中定义。**校验失败会触发回退**——别想着「越界一点点没关系」。

---

## 5. 颜色字面量

`roles.*` 和 `gradients.*` 中的颜色字符串必须是以下三种之一（pattern 见 schema `$defs/color`）：

```
#RRGGBB             // alpha 默认为 255
#RRGGBBAA           // 显式 alpha
rgba(R, G, B, A)    // R/G/B 为 0..255 整数，A 为 0..1 浮点
```

**渐变** (`gradients.*`) 是颜色数组，长度必须 2..8。stop 之间的位置由 `SkinRenderer::SampleGradient` 等距插值，**不支持显式 stop 位置**。需要非等距渐变，请增加冗余 stop（例如 `[A, A, B]` 让 A 占据前半段）。

---

## 6. 资源（assets）规则

`assets` 是可选对象，键固定为 `preview` / `displayFont` / `bodyFont` / `backgroundTexture`，值为**相对皮肤目录的路径**。

校验规则（`SkinManager::validateAsset`）：

- 必须是相对路径，**不**接受绝对路径（含 Windows 盘符）和 `..` 段
- 字符白名单：字母、数字、`_`、`.`、`/`、空格、`-`
- 规范化后必须仍位于皮肤目录内（防穿越）
- 扩展名白名单：`.ttf` / `.ttc` / `.otf` / `.png` / `.svg`
- 单文件 ≤ 4 MB
- 整个皮肤目录递归累计 ≤ 16 MB

`skin.json` 本体不计入资源，但本身有 256 KB 上限。

---

## 7. 编辑工作流

### 7.1 修改现有皮肤

1. 启动应用，确认 Settings → Appearance → `Hot Reload [x]` 已勾选（默认开启）
2. 用任意文本编辑器修改 `source/UI/skins/<id>/skin.json`
3. 保存后等约 200 ms（`motion.reloadDebounceMs`），UI 应自动刷新；状态行显示 `Status: WATCHING`
4. 写错字段或越界 → 状态行变红，显示 `INVALID - USING PREVIOUS SKIN`，旧皮肤仍生效

### 7.2 新增皮肤

```bash
cp -r source/UI/skins/cyberpunk-neon source/UI/skins/<your-id>
# 编辑 source/UI/skins/<your-id>/skin.json：
#   - "id": "<your-id>"   ← 必须与目录名一致
#   - "name": "Your Name"
#   - "version": "1.0.0"
#   修改 roles / metrics / surfaces / motion / decoration ...
```

重启应用 → Settings → Appearance → 皮肤下拉框中选择新皮肤。选定后 `Config::skinId` 会写回 `fluxplayer.ini`。

### 7.3 验证清单（提 PR 前自查）

- [ ] `id` 字段与所在目录名**完全一致**
- [ ] `schemaVersion` 为 `1`，`compatibility.skinApi` 为 `1`
- [ ] `compatibility.surfaces` 包含 `home`、`opening`、`player`、`hud`、`settings`、`popup`、`subtitle`
- [ ] `player` 仍容纳 `Download` 百分比/速度/大小/`ETA` 与独立 `REC V`、`REC A`
- [ ] 所有颜色字面量符合 §5 的格式
- [ ] 所有数值字段在 schema 规定的范围内
- [ ] 引用的资源文件存在、扩展名合法、大小合理
- [ ] 准备 `preview.svg`（推荐 16:9，皮肤切换器会显示）
- [ ] 在 macOS / Windows 至少一个平台跑通启动 + 切换 + 热加载
- [ ] 切换皮肤时不重启播放、不重置进度

---

## 8. 加新字段的规范

如果你需要让皮肤系统接管一个新的视觉参数：

1. **先想清楚是「全局」还是「容器局部」**（参考 §3）。如果它会影响多个 Surface 的多个控件 → 全局；如果只在某个 Surface 的某段绘制中用 → 容器局部。
2. 在 `skin.schema.json` 中新增字段，**显式给定 minimum/maximum、pattern、enum**——校验越严越好。
3. 在 `Skin.h` 的对应结构体里加 POD 字段。
4. 在 `SkinManager.cpp::loadFromString` 中加一行 `requireXxx(...)`。
5. 在消费侧使用：
   - **全局**：在 `SkinRenderer::ApplyImGuiStyle` 中写入对应 `ImGuiStyle` 字段。
   - **容器局部**：在具体 Surface 的渲染函数中通过 `PushStyleVar` 或直接当数值用，绝不落入全局 `ImGuiStyle`。
6. 同步更新 `source/UI/skins/cyberpunk-neon/skin.json`（内置皮肤必须包含所有必填字段）。
7. 在本 README §3.1 或 §3.2 增加一行说明，**写清楚归属与影响范围**。

> **决不要为了「省事」复用语义不同的字段。** §3 的 dockPadding 反例就是这么来的。

---

## 9. 常见错误案例

### 9.1 dock 按钮文字撑出边框（已修复）

**症状**：底部 dock 中 `REC V` / `REC A` 等按钮的文字位置偏移、上下不居中。
**原因**：早期把 Dock 容器 padding 当作全局 `FramePadding`。较大的垂直边距配合按钮固定高度会令文字被裁切。
**修复**：`FramePadding` 全局保持 `(4, 3)`；`surfaces.player.dockPaddingX/Y` 只由 Dock 容器自己消费。
**教训**：「容器局部」的字段不能写入全局 `ImGuiStyle`。详见 §3.4.5。

### 9.2 OPEN URL 按钮比输入框矮（已修复）

**症状**：HomeScreen URL 行，输入框高度 ~36 px，紧邻的 `OPEN URL` 按钮只有 ~20 px。
**原因**：输入框 `PushStyleVar(FramePadding, (14, 10))` → 高度 `lineH + 20`；按钮在 `PopStyleVar` 之后才渲染，用了全局默认 `FramePadding(4, 3)` → 高度 `lineH + 6`。同一行控件用了不同的 `FramePadding.y`。
**修复**：把按钮放进同一段 `Push/Pop FramePadding(14, 10)` 块内（或显式再 push 一次相同值）。
**教训**：见 §3.4.1「同一行的控件必须共用 `FramePadding`」。

### 9.3 设置菜单太窄、文字被裁切（已修复）

**症状**：底部齿轮弹出的设置菜单只有 280 px 宽，`Cyberpunk Neon / BUI...`、`RESTORE DEF...` 都被截断；用户必须把菜单当主窗口尝试拉宽，但它是 `NoResize`。
**原因**：用 dropdown 形态承载了「皮肤切换 + 三个长按钮 + 状态行」共 6 项交互；280 px 远不够 combo 全宽 + 按钮组横向布局。
**修复**：改为居中模态窗口 `renderSettingsModal`，宽度由 `min(surfaces.settings.maxWidth, displayW * surfaces.settings.widthRatio)` 决定，三栏按钮等分；半透明遮罩 + ESC/关闭按钮退出。
**教训**：见 §3.4.3「弹窗的最小尺寸约束」。

### 9.4 改字段后 UI 不变化

可能原因：
- 没保存文件 / 编辑器有 `.swp` 临时文件干扰 mtime 监听 → 等 `motion.reloadDebounceMs` 完整窗口
- JSON 语法错误 → 看 Appearance 状态行，应显示 `INVALID - USING PREVIOUS SKIN`
- 改了 `compatibility.surfaces` 之类的字段，但去掉了必需值 → 触发回退

### 9.5 自定义皮肤启动后被回退

- 检查目录名与 `id` 是否一致（`SkinManager` 用目录名作为 `id` 索引）
- 看 `lastError()`：通常是某个数值越界、颜色字面量错误、必填字段缺失
- 资源路径写错（`..` 段、绝对路径、扩展名不在白名单）

---

## 10. 与代码的对应关系

| 关注点 | 看哪里 |
| --- | --- |
| 字段权威定义 | `source/UI/skin.schema.json` |
| 解析与校验逻辑 | `src/ui/SkinManager.cpp::loadFromString` |
| 全局样式映射 | `src/ui/SkinRenderer.cpp::ApplyImGuiStyle` |
| 装饰绘制助手 | `src/ui/SkinRenderer.cpp` 其他函数 |
| Home 消费点 | `src/ui/HomeScreen.cpp::renderBackground` / `renderUI` |
| Player dock 消费点 | `src/ui/Controller.cpp::renderBottomOverlay` 及其下属函数 |
| 设置 / Appearance 模态 | `src/ui/Controller.cpp::renderSettingsModal`（参考 mockup_skin_settings.svg） |

任何对皮肤系统的改动都应同步更新本 README 的相关章节。
