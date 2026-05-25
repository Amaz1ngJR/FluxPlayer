# Microsoft WebView2 SDK Header

仅放置 FluxPlayer 内置登录窗口（`src/utils/WebLogin_win.cpp`）所需的官方头文件。

## 来源

- 包名：`Microsoft.Web.WebView2`
- 版本：`1.0.3967.48`（撰写时 NuGet 最新稳定版）
- 下载：`https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3967.48/microsoft.web.webview2.1.0.3967.48.nupkg`
- 提取路径：`build/native/include/WebView2.h`

## 内容

- `include/WebView2.h` — WebView2 COM 接口定义，与 macOS 端调用 `WebKit.framework` 等价
- `LICENSE.txt` — 微软官方 LICENSE（BSD 风格，允许源码重分发）

## 不附带的部分

- `WebView2LoaderStatic.lib`：FluxPlayer 通过 `LoadLibraryW(L"WebView2Loader.dll")` 动态加载，不需要静态库
- `WebView2Loader.dll`：由系统 WebView2 Runtime 提供（Win10 21H1+ / Win11 已预装；旧系统通过 Microsoft Edge 安装）
- `WebView2EnvironmentOptions.h`：当前未使用（FluxPlayer 不自定义环境选项）

## 升级方式

需要升级时替换 `include/WebView2.h` 与 `LICENSE.txt` 为目标版本即可，无需修改构建脚本。
