# Windows 打包脚本：构建 FluxPlayer 并用 Inno Setup 生成安装程序
#
# 依赖：
#   - CMake 3.16+       构建系统（https://cmake.org）
#   - MinGW-w64 或 MSVC C++ 编译器
#   - Inno Setup 6      安装包制作工具（https://jrsoftware.org/isinfo.php）
#   - ImageMagick       可选，用于 PNG->ICO 转换（https://imagemagick.org）
#
# 图标优先级：
#   1. source\pic.ico（已有则直接使用，跳过转换）
#   2. source\pic.png（需要 ImageMagick 转换，没有则跳过并警告）
#
# 用法：
#   .\scripts\package_windows.ps1
#   .\scripts\package_windows.ps1 -InnoSetup "D:\InnoSetup6\ISCC.exe"
#
# 输出：
#   dist\FluxPlayer-0.1.0-Setup.exe

param(
    # Inno Setup 编译器路径，默认为标准安装位置
    [string]$InnoSetup = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$root  = Split-Path $PSScriptRoot -Parent
$build = "$root\build"

# 从 CMakeLists.txt 读取版本号，发版只需改那一处
$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(FluxPlayer VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Matches[0].Groups[1].Value

# ── 1. 图标处理 ───────────────────────────────────────────────────────────────
$png = "$root\source\pic.png"
$ico = "$root\source\pic.ico"
if (-not (Test-Path $ico)) {
    # pic.ico 不存在时尝试用 ImageMagick 从 pic.png 转换
    if (Get-Command magick -ErrorAction SilentlyContinue) {
        magick convert $png -define icon:auto-resize="256,128,64,48,32,16" $ico
    } else {
        Write-Warning "ImageMagick not found. Place source\pic.ico manually to use a custom icon."
    }
}

# ── 2. CMake 构建（Release 模式）──────────────────────────────────────────────
cmake -S $root -B $build -DCMAKE_BUILD_TYPE=Release
cmake --build $build --config Release

# ── 3. 创建输出目录 ───────────────────────────────────────────────────────────
New-Item -ItemType Directory -Force "$root\dist" | Out-Null

# ── 3.5 复制 MinGW 运行时 DLL ─────────────────────────────────────────────────
$mingwDlls = @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")
$mingwBin = Split-Path (Get-Command gcc -ErrorAction Stop).Source -Parent
foreach ($dll in $mingwDlls) {
    $src = Join-Path $mingwBin $dll
    if (Test-Path $src) { Copy-Item $src $build -Force }
    else { Write-Warning "Not found: $src" }
}

# ── 4. Inno Setup 打包 ────────────────────────────────────────────────────────
# package_windows.iss 定义了安装包内容、快捷方式、卸载程序等
# 通过 /DAppVersion 将版本号注入 .iss，避免在两处维护
& $InnoSetup "/DAppVersion=$version" "$PSScriptRoot\package_windows.iss"

Write-Host "Done: $root\dist\FluxPlayer-$version-Setup.exe"
