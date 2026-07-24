# setup_env.ps1 — Windows MinGW + xmake 环境配置脚本
# 安装xmake : 
#Invoke-WebRequest `
#  -Uri "https://github.com/xmake-io/xmake/releases/download/v2.9.9/xmake-bundle-v2.9.9.win64.exe" `
#  -OutFile "C:\Xmake\xmake.exe"
# 使用方式：在 PowerShell 中运行 D:\tools\setup_env.ps1
# 首次使用需执行：Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
# 清掉这个损坏缓存目录: Remove-Item -Recurse -Force "$env:TEMP\.xmake0\2.9.9+dev.a6af349ad"Remove-Item -Recurse -Force "$env:TEMP\.xmake0\2.9.9+dev.a6af349ad"


$mingw_path = "C:\msys64\mingw64\bin"
$xmake_path = "C:\Xmake\"

# 添加 MinGW 到 PATH
if (Test-Path $mingw_path) {
    $env:PATH = "$mingw_path;" + $env:PATH
    Write-Host "Added MinGW to PATH: $mingw_path" -ForegroundColor Green
} else {
    Write-Error "MinGW path not found: $mingw_path"
    exit 1
}

# 添加 xmake 到 PATH
if (Test-Path $xmake_path) {
    $env:PATH = "$xmake_path;" + $env:PATH
    Write-Host "Added xmake to PATH: $xmake_path" -ForegroundColor Green
} else {
    Write-Error "xmake path not found: $xmake_path"
    exit 1
}

# 验证工具
Write-Host "Checking gcc..." -ForegroundColor Cyan
$gcc_version = gcc --version 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error "gcc not found!"; exit 1 }
Write-Host $gcc_version[0] -ForegroundColor Green

Write-Host "Checking g++..." -ForegroundColor Cyan
$gpp_version = g++ --version 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error "g++ not found!"; exit 1 }
Write-Host $gpp_version[0] -ForegroundColor Green

Write-Host "Checking xmake..." -ForegroundColor Cyan
$xmake_version = xmake --version 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error "xmake not found!"; exit 1 }
Write-Host $xmake_version[0] -ForegroundColor Green
if ($xmake_version[0] -notmatch "xmake v2\.9\.") {
    Write-Error "FluxPlayer currently requires xmake 2.9.x. xmake 3.x can fail with: cannot import module: private.check.checkers.cuda.devlink"
    Write-Error "Install xmake 2.9.x, then rerun this script."
    exit 1
}

Write-Host ""
Write-Host "All tools are ready!" -ForegroundColor Green

# 配置 xmake 使用 MinGW。-o 显式指定构建根目录，既与 xmake.lua 的默认值一致，
# 也能覆盖旧版 .xmake 配置中遗留的 build 路径，确保缓存和对象文件进入 build\windows。
Write-Host "Configuring xmake with MinGW toolchain..." -ForegroundColor Cyan
New-Item -Path ".xmake\windows\x64" -ItemType Directory -Force | Out-Null
xmake f -p windows -o build/windows --toolchain=mingw --mingw="C:\msys64\mingw64" --cross="x86_64-w64-mingw32-" -y

if ($LASTEXITCODE -eq 0) {
    Write-Host "xmake configuration succeeded!" -ForegroundColor Green
} else {
    Write-Error "xmake configuration failed!"
    exit 1
}
