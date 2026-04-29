param(
    [string]$InnoSetup = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = "$root\build"

# 1. 图标转换：source/pic.png -> source/pic.ico（需要 ImageMagick）
$png = "$root\source\pic.png"
$ico = "$root\source\pic.ico"
if (-not (Test-Path $ico)) {
    magick convert $png -define icon:auto-resize="256,128,64,48,32,16" $ico
}

# 2. 构建
cmake -S $root -B $build -DCMAKE_BUILD_TYPE=Release
cmake --build $build --config Release

# 3. 创建输出目录
New-Item -ItemType Directory -Force "$root\dist" | Out-Null

# 4. 打包
& $InnoSetup "$PSScriptRoot\package_windows.iss"

Write-Host "Done: $root\dist\FluxPlayer-0.1.0-Setup.exe"
