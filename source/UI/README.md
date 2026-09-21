# FluxPlayer 单文件 Lua 皮肤规范

`source/UI/skins/` 保存可运行皮肤。一个皮肤目录只有一个可选择入口：

```text
source/UI/skins/
  <id>/
    <id>.lua        # 唯一运行时入口
    preview.svg     # 可选预览
    mockup_*.svg    # 可选设计稿
    fonts/          # 可选
    textures/       # 可选
```

目录名、Lua 文件名和脚本返回的 `id` 必须相同。`skin.json` 已弃用，不参与扫描、加载、打包或热重载。

默认实例：

```text
skins/cyberpunk-neon/cyberpunk-neon.lua
```

`minimal-lua/minimal-lua.lua` 是第二套完整的单文件示例皮肤，不依赖其他文件。

## 加载优先级

| 来源 | 路径 | 优先级 |
| --- | --- | --- |
| User | `<AppData>/skins/<id>/<id>.lua` | 最高 |
| Dev | `<cwd>/source/UI/skins/<id>/<id>.lua` | 中 |
| BuiltIn | `<exe>/resources/skins/<id>/<id>.lua` | 最低 |

请求皮肤加载失败时依次尝试低优先级副本，再回退 `cyberpunk-neon`。所有 Lua 副本都失败时只使用编译期紧急快照保证程序可启动。

## 文件内容

`<id>.lua` 同时定义：

- 元数据：`schemaVersion`、`id`、`name`、`version`；
- 语义颜色：`roles`；
- 渐变：`gradients`；
- 圆角、间距、尺寸、透明度：`metrics`；
- Home、Opening、Player、HUD、Settings、Popup、Subtitle 参数：`surfaces`；
- 时序：`motion`；
- 字体 token：`typography`；
- 装饰开关：`decoration`；
- 包内资源：`assets`；
- 可执行 UI：`surfaces.<name>.render`。

完整格式以 `skins/cyberpunk-neon/cyberpunk-neon.lua` 为准，FluxUI API 与技术细节见 `../../docs/v0.8.5 Lua皮肤系统技术方案.md`。

## 热加载工作流

1. 在 Settings > Appearance 启用 `Hot Reload`。
2. 直接修改 `source/UI/skins/<id>/<id>.lua`。
3. 保存后等待 `motion.reloadDebounceMs`。
4. 有效修改在后续帧整体生效；无效修改保留上一份有效 VM 和快照。
5. 在 Appearance 状态栏查看错误，也可点击 `RELOAD NOW`。

可动态修改颜色、尺寸、间距、动画和 Surface 函数。无需重启播放器，也不会重置播放状态。

## Lua/C++ 边界

Lua 拥有皮肤业务和 UI 描述。C++ 只提供基础服务：

- 沙箱 VM、内存和指令配额；
- FluxUI 控件树、布局、输入和绘制后端；
- 白名单动作到播放器功能的桥接；
- 资源路径/大小校验；
- 热重载、快照交换和错误回退。

脚本不能访问文件、网络、系统 API、ImGui 类型或原生模块。设置弹窗仍为原生实现，不属于已迁移的 Lua 页面；其打开时底层 Player Surface 保持可见但不响应输入。

## Player 设计坐标

默认皮肤以 `mockup_player.svg` 的 1440×900 为参考，缩放系数为 `min(1, width/1440, height/900)`（窗口逻辑尺寸）。左侧下载/硬件组贴左、播放组居中、右侧工具及弹出菜单贴右，只缩小、不随大窗口放大；背景与顶部光带铺满宽度，宿主窗口 padding 为零，避免边缘被裁掉。底栏高 88，按钮行高 38、距底 7；进度条参考为 `(8,819,1224,16)`，实际宽度随窗口伸展，距宽 192 的时间列 8。时间列距右 8，使用 12px 等宽 mono 字体和显式基线 y=831，格式为 `HH:MM:SS / HH:MM:SS`。字幕共用缩放函数，避免底栏调整后间距漂移。

INFO / STATS 面板的标题、正文和关闭文字统一由 Lua 显式选择 `mono`，基准字号 12px，使用 SVG 的文字基线（标题 y=29），不再套用默认正文或文本框垂直居中。字体优先 Consolas，缺失时使用系统等宽字体；实际路径见启动日志。回退字体不保证与设计稿查看器的 monospace 解析一致，仍需截图对比，不能据此宣称像素级还原。

尺寸和坐标属于 Lua，不应在 C++ 中重复写一份 Player 布局。`metrics.size.bottomDockHeight` 是原生回退使用的 token，默认值同为 88。SVG 不指定固定倍速上限；可选档位来自每帧 `player.maxSpeed`，提交倍速时由 C++ 再校验。具体数据接口见 API 文档。

## 资源规则

`assets` 路径必须相对皮肤目录，canonical 后仍位于该目录内。允许：

- `.ttf`、`.ttc`、`.otf`
- `.png`、`.svg`

单资源不超过 4 MB，整个皮肤目录不超过 16 MB。设计稿 SVG 可以保留，但只有 Lua 中显式引用的资源参与运行时。

## 构建与发布

CMake、xmake、macOS 打包脚本和安装规则都会复制整个皮肤目录到 `resources/skins/<id>`，因此 `<id>.lua`、预览和官方 SVG 可保持一致。构建流程会先清理旧目标，避免已删除的 `skin.json` 残留并遮蔽验证结果。

## 验收清单

- `<id>/<id>.lua` 存在，三处 id 完全一致；
- Lua 返回完整静态 token；
- Home 必要动作可达；
- Opening、Player、HUD、Settings、Popup、Subtitle 参数合法；
- 热加载成功时 generation 增加；
- Lua 语法或范围错误时保留上一有效皮肤；
- 打包目录中没有 `skin.json`；
- Lua `assets.preview` 指向现存的预览资源；需要保留的 `mockup_*.svg` 未丢失。
