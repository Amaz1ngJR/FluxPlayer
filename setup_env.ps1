# setup_env.ps1 — Windows MinGW + xmake 环境配置脚本
# 使用方式：在 PowerShell 中运行 D:\tools\setup_env.ps1
# 首次使用需执行：Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

$mingw_path = "D:\tools\mingw64\bin"
$xmake_path = "D:\tools"

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

Write-Host ""
Write-Host "All tools are ready!" -ForegroundColor Green

# 配置 xmake 使用 MinGW
Write-Host "Configuring xmake with MinGW toolchain..." -ForegroundColor Cyan
xmake f -p windows --toolchain=mingw --mingw="D:\tools\mingw64" --cross="x86_64-w64-mingw32-" -c -y

if ($LASTEXITCODE -eq 0) {
    Write-Host "xmake configuration succeeded!" -ForegroundColor Green
} else {
    Write-Error "xmake configuration failed!"
    exit 1
}
