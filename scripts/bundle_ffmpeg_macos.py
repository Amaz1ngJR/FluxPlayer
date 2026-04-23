#!/usr/bin/env python3
"""
bundle_ffmpeg_macos.py — 从 Homebrew 收集 FFmpeg dylib 及所有传递依赖，
修复 install_name 为 @loader_path，输出到 third_party/ffmpeg-macos/

用法：在项目根目录执行 python3 scripts/bundle_ffmpeg_macos.py
前置条件：brew install ffmpeg@4
"""

import subprocess, os, re, shutil, sys
from pathlib import Path

# ========================
# 配置
# ========================
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
OUTPUT_DIR = PROJECT_ROOT / "third_party" / "ffmpeg-macos"
LIB_DIR = OUTPUT_DIR / "lib"
INC_DIR = OUTPUT_DIR / "include"

# 获取 Homebrew FFmpeg 路径
result = subprocess.run(["brew", "--prefix", "ffmpeg@4"], capture_output=True, text=True)
if result.returncode != 0 or not Path(result.stdout.strip()).is_dir():
    print("错误：未找到 Homebrew 安装的 ffmpeg@4")
    print("请先执行：brew install ffmpeg@4")
    sys.exit(1)

FFMPEG_PREFIX = Path(result.stdout.strip())

# FluxPlayer 实际链接的 FFmpeg 库
FFMPEG_LIBS = [
    FFMPEG_PREFIX / "lib" / "libavformat.dylib",
    FFMPEG_PREFIX / "lib" / "libavcodec.dylib",
    FFMPEG_PREFIX / "lib" / "libavutil.dylib",
    FFMPEG_PREFIX / "lib" / "libswscale.dylib",
    FFMPEG_PREFIX / "lib" / "libswresample.dylib",
]


def get_homebrew_deps(dylib_path: str) -> list[str]:
    """解析 otool -L，返回所有 /opt/homebrew/ 下的依赖路径"""
    result = subprocess.run(["otool", "-L", dylib_path], capture_output=True, text=True)
    deps = []
    for line in result.stdout.strip().split("\n")[1:]:
        match = re.match(r"\s+(/opt/homebrew/\S+)", line)
        if match:
            deps.append(match.group(1))
    return deps


# Homebrew lib 搜索路径（用于解析 @rpath 引用）
HOMEBREW_LIB_DIRS = [
    Path("/opt/homebrew/lib"),
    FFMPEG_PREFIX / "lib",
]


def get_rpath_deps(dylib_path: str) -> list[tuple[str, str]]:
    """解析 otool -L，返回所有 @rpath/ 引用及其在 Homebrew 中的实际路径。
    返回 [(otool 中的原始引用, Homebrew 实际路径), ...]"""
    result = subprocess.run(["otool", "-L", dylib_path], capture_output=True, text=True)
    deps = []
    for line in result.stdout.strip().split("\n")[1:]:
        match = re.match(r"\s+(@rpath/(\S+))", line)
        if match:
            ref, name = match.group(1), match.group(2)
            for lib_dir in HOMEBREW_LIB_DIRS:
                candidate = lib_dir / name
                if candidate.exists():
                    deps.append((ref, str(candidate.resolve())))
                    break
    return deps


def get_install_name(dylib_path: str) -> str:
    """获取 dylib 的 install_name（otool -D 输出第二行）"""
    result = subprocess.run(["otool", "-D", dylib_path], capture_output=True, text=True)
    lines = result.stdout.strip().split("\n")
    return lines[1] if len(lines) > 1 else ""


# ========================
# 递归收集所有 Homebrew 依赖
# ========================
print("=== 收集 FFmpeg dylib 依赖树 ===")

visited: dict[str, str] = {}  # 真实路径 -> 目标文件名

def collect_deps(dylib_path: str):
    """递归收集 dylib 及其所有 Homebrew 传递依赖"""
    real = str(Path(dylib_path).resolve())
    if real in visited:
        return
    if not os.path.isfile(real):
        return
    target_name = os.path.basename(real)
    visited[real] = target_name

    # 收集 /opt/homebrew/ 绝对路径引用
    for dep in get_homebrew_deps(real):
        dep_real = str(Path(dep).resolve()) if os.path.exists(dep) else ""
        if dep_real and os.path.isfile(dep_real):
            collect_deps(dep_real)

    # 收集 @rpath/ 引用（部分 Homebrew 库用 @rpath 引用兄弟库）
    for _ref, dep_real in get_rpath_deps(real):
        if dep_real and os.path.isfile(dep_real):
            collect_deps(dep_real)


for lib in FFMPEG_LIBS:
    if not lib.exists():
        print(f"错误：找不到 {lib}")
        sys.exit(1)
    collect_deps(str(lib))

print(f"共收集 {len(visited)} 个 dylib")

# ========================
# 清理并复制
# ========================
print(f"=== 复制 dylib 到 {LIB_DIR} ===")
if OUTPUT_DIR.exists():
    shutil.rmtree(OUTPUT_DIR)
LIB_DIR.mkdir(parents=True)
INC_DIR.mkdir(parents=True)

for real_path, target_name in visited.items():
    dest = LIB_DIR / target_name
    shutil.copy2(real_path, dest)
    os.chmod(dest, 0o755)

# 为 FFmpeg 核心库创建无版本号的符号链接（链接器需要 -lavformat 能找到 libavformat.dylib）
for lib in FFMPEG_LIBS:
    real = str(lib.resolve())
    versioned = visited[real]
    # libavformat.58.76.100.dylib -> libavformat.dylib
    short = re.sub(r"\.\d+.*\.dylib$", ".dylib", versioned)
    # libavformat.58.76.100.dylib -> libavformat.58.dylib
    major = re.sub(r"(\.\d+)\.\d+(\.\d+)*\.dylib$", r"\1.dylib", versioned)
    if short != versioned:
        (LIB_DIR / short).unlink(missing_ok=True)
        (LIB_DIR / short).symlink_to(versioned)
    if major != versioned and major != short:
        (LIB_DIR / major).unlink(missing_ok=True)
        (LIB_DIR / major).symlink_to(versioned)

# ========================
# 修复 install_name
# ========================
print("=== 修复 install_name ===")

# 构建 "原始引用路径 -> 新文件名" 的映射
name_map: dict[str, str] = {}

for real_path, target_name in visited.items():
    dest = str(LIB_DIR / target_name)
    # 记录 install_name -> 目标文件名
    original_id = get_install_name(dest)
    if original_id:
        name_map[original_id] = target_name
    # 记录 otool -L 中出现的引用路径（可能是 Cellar 路径或 opt 路径）
    for dep in get_homebrew_deps(real_path):
        dep_real = str(Path(dep).resolve()) if os.path.exists(dep) else ""
        if dep_real in visited:
            name_map[dep] = visited[dep_real]

# 对每个 dylib 执行 install_name_tool
for real_path, target_name in visited.items():
    target_file = str(LIB_DIR / target_name)

    # 修改自身的 install_name（-id）
    subprocess.run([
        "install_name_tool", "-id", f"@loader_path/{target_name}", target_file
    ], check=True)

    # 修改对其他 Homebrew dylib 的引用（-change）：/opt/homebrew/ 绝对路径
    for old_ref in get_homebrew_deps(target_file):
        new_name = name_map.get(old_ref)
        if new_name:
            subprocess.run([
                "install_name_tool", "-change", old_ref,
                f"@loader_path/{new_name}", target_file
            ], check=True)

    # 修改 @rpath/ 引用为 @loader_path/（部分 Homebrew 库用 @rpath 引用兄弟库）
    for rpath_ref, rpath_real in get_rpath_deps(target_file):
        if rpath_real in visited:
            subprocess.run([
                "install_name_tool", "-change", rpath_ref,
                f"@loader_path/{visited[rpath_real]}", target_file
            ], check=True)

# ========================
# 重新签名（install_name_tool 会使原签名失效，macOS 拒绝加载未签名 dylib）
# ========================
print("=== 重新签名 dylib ===")
for real_path, target_name in visited.items():
    target_file = str(LIB_DIR / target_name)
    subprocess.run([
        "codesign", "--force", "-s", "-", target_file
    ], check=True, capture_output=True)

# ========================
# 复制 FFmpeg 头文件
# ========================
print("=== 复制 FFmpeg 头文件 ===")
for header_dir in sorted(FFMPEG_PREFIX.glob("include/lib*")):
    if header_dir.is_dir():
        shutil.copytree(header_dir, INC_DIR / header_dir.name)

# ========================
# 验证
# ========================
print("\n=== 验证 install_name ===")
fail = False
for real_path, target_name in visited.items():
    target_file = str(LIB_DIR / target_name)
    # 检查残留的 /opt/homebrew 引用
    for dep in get_homebrew_deps(target_file):
        print(f"警告：{target_name} 仍有 Homebrew 引用：{dep}")
        fail = True
    # 检查残留的 @rpath 引用
    for rpath_ref, _ in get_rpath_deps(target_file):
        print(f"警告：{target_name} 仍有 @rpath 引用：{rpath_ref}")
        fail = True

if not fail:
    print("所有 dylib 的 install_name 已修复完成")

# 统计
total_size = sum(f.stat().st_size for f in LIB_DIR.iterdir() if f.is_file())
total_count = sum(1 for f in LIB_DIR.iterdir() if f.suffix == ".dylib" and not f.is_symlink())
print(f"\n=== 完成 ===")
print(f"输出目录：{OUTPUT_DIR}")
print(f"dylib 数量：{total_count}")
print(f"总大小：{total_size / 1024 / 1024:.1f} MB")
