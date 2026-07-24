# Windows 打包脚本：构建 FluxPlayer 并用 Inno Setup 生成安装程序
#
# 依赖：
#   - CMake 3.16+       构建系统（https://cmake.org）
#   - MSYS2 MinGW-w64   与 xmake Windows 构建使用同一套 GCC/Ninja 工具链
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
#   .\scripts\package_windows.ps1 -CMake "C:\msys64\mingw64\bin\cmake.exe"
#
# 输出：
#   dist\FluxPlayer-<版本号>-Setup.exe

param(
    # Inno Setup 编译器路径，默认为标准安装位置
    [string]$InnoSetup = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",

    # CMake 可执行文件路径。留空时依次检查 PATH、MSYS2 和官方默认安装目录。
    [string]$CMake = ""
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$root  = Split-Path $PSScriptRoot -Parent
# CMake 缓存和中间文件固定放在 build\cmake；xmake 使用 build\windows。
# 两套构建系统只在 build\bin 共享最终运行产物，该路径由 CMakeLists.txt/xmake.lua
# 统一配置。这样既允许最后一次构建覆盖 exe，又不会混用对象文件或生成器缓存。
$build = "$root\build\cmake"
$t0 = Get-Date

function Step($name, $block) {
    $t = Get-Date
    # Windows PowerShell 的 ErrorActionPreference 不会把 exe 返回的非零退出码转换成异常。
    # 每一步开始前清零 LASTEXITCODE，执行后显式检查，避免 Inno Setup/CMake 已失败，
    # 脚本却继续打印 Done 并以 0 退出，产生一个实际上不存在或不完整的安装包。
    $global:LASTEXITCODE = 0
    & $block
    $exitCode = $global:LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Step '$name' failed with exit code $exitCode."
    }
    Write-Host ("[{0}s] {1}" -f [math]::Round(((Get-Date) - $t).TotalSeconds, 1), $name)
}

function Resolve-CMakeExecutable([string]$explicitPath) {
    # 显式参数优先，方便使用非标准安装目录或指定某个 CMake 版本。
    if ($explicitPath) {
        if (-not (Test-Path -LiteralPath $explicitPath -PathType Leaf)) {
            throw "CMake executable not found: $explicitPath"
        }
        return (Resolve-Path -LiteralPath $explicitPath).Path
    }

    # 正常安装并已加入 PATH 时直接复用当前环境。
    $command = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    # setup_env.ps1 使用 MSYS2 MinGW；另外兼容 CMake 官方安装器的默认目录。
    $candidates = @(
        "C:\msys64\mingw64\bin\cmake.exe"
        "C:\Program Files\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "CMake 3.16+ not found. Install it or pass -CMake <path-to-cmake.exe>."
}

$cmakeExe = Resolve-CMakeExecutable $CMake

# CMake 即使通过绝对路径启动，配置阶段仍需要从 PATH 查找编译器、Ninja 以及
# MSYS2 运行库。这里只修改当前打包脚本及其子进程的环境，不会永久修改系统 PATH。
$mingwBin = "C:\msys64\mingw64\bin"
# setup_env.ps1 为 xmake 配置了 x86_64-w64-mingw32- cross 前缀；CMake 也使用
# 完全相同的编译器入口，避免同一套 GCC 因 gcc/g++ 与带前缀文件名不同而刷新缓存。
$cCompiler = "$mingwBin\x86_64-w64-mingw32-gcc.exe"
$cxxCompiler = "$mingwBin\x86_64-w64-mingw32-g++.exe"
$ninjaExe = "$mingwBin\ninja.exe"
$hasCCompiler = Test-Path -LiteralPath $cCompiler -PathType Leaf
$hasCxxCompiler = Test-Path -LiteralPath $cxxCompiler -PathType Leaf
$hasNinja = Test-Path -LiteralPath $ninjaExe -PathType Leaf
$hasMingwCompiler = $hasCCompiler -and $hasCxxCompiler -and $hasNinja
if (-not $hasMingwCompiler) {
    throw "MSYS2 MinGW toolchain is incomplete. Expected gcc.exe, g++.exe and ninja.exe in $mingwBin."
}
$mingwAlreadyInPath = ($env:Path -split ";") -contains $mingwBin
if (-not $mingwAlreadyInPath) {
    $env:Path = "$mingwBin;$env:Path"
}

if (-not (Test-Path -LiteralPath $InnoSetup -PathType Leaf)) {
    throw "Inno Setup compiler not found: $InnoSetup"
}

Write-Host "Using CMake: $cmakeExe"

# CMake 和 xmake 都维护版本宏。打包前强制校验，防止 UI、安装包文件名和
# xmake 产物显示不同版本；修改版本时必须同时更新两个构建文件。
$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(FluxPlayer VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Matches[0].Groups[1].Value
$xmakeVersion = (Select-String -Path "$root\xmake.lua" -Pattern 'local version = "([0-9]+\.[0-9]+\.[0-9]+)"').Matches[0].Groups[1].Value
if ($version -ne $xmakeVersion) {
    throw "Build version mismatch: CMake=$version, xmake=$xmakeVersion. Update both build files before packaging."
}

# ── 1. 图标处理 ───────────────────────────────────────────────────────────────
$png = "$root\source\pic.png"
$ico = "$root\source\pic.ico"
Step "icon" {
    if (-not (Test-Path $ico)) {
        if (Get-Command magick -ErrorAction SilentlyContinue) {
            magick convert $png -define icon:auto-resize="256,128,64,48,32,16" $ico
        } else {
            Write-Warning "ImageMagick not found. Place source\pic.ico manually to use a custom icon."
        }
    }
}

# ── 2. CMake 构建（Release 模式）──────────────────────────────────────────────
# 打包流程每次都会先显式执行 configure，所以不需要 Ninja 再启动 CMake 检查 glob。
# 当前 MSYS2 CMake 4.4 在 Ninja 子进程中执行 VerifyGlobs.cmake 时可能长期阻塞；
# 关闭自动再生成只作用于这个打包专用目录，新增源文件仍会在上面的 configure 中发现。
Step "CMake configure" {
    & $cmakeExe -S $root -B $build -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
        "-DCMAKE_C_COMPILER=$cCompiler" `
        "-DCMAKE_CXX_COMPILER=$cxxCompiler" `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_SUPPRESS_REGENERATION=ON
}
Step "CMake build" { & $cmakeExe --build $build --config Release }

# ── 3. cmake --install 到干净的 staging 目录 ─────────────────────────────────
# staging 与 build 完全隔离，不受 xmake 或其他工具残留 DLL 的影响
$stage = "$root\dist\staging"
Step "cmake install (staging)" {
    & $cmakeExe --install $build --config Release --prefix $stage
}

# ── 4. Inno Setup 打包 ────────────────────────────────────────────────────────
# package_windows.iss 定义了安装包内容、快捷方式、卸载程序等
# 通过 /DAppVersion 将版本号注入 .iss，避免在两处维护
Step "inno setup" { & $InnoSetup "/DAppVersion=$version" "$PSScriptRoot\package_windows.iss" }

Write-Host ("Total: {0}s" -f [math]::Round(((Get-Date) - $t0).TotalSeconds, 1))
Write-Host "Done: $root\dist\FluxPlayer-$version-Setup.exe"
