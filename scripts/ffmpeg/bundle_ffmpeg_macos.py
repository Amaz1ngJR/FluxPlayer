#!/usr/bin/env python3
"""
收集 macOS FFmpeg 动态库依赖，修复安装路径并签名。
支持用项目补丁构建的 libavformat 替换 Homebrew 对应库，其余编码能力保持不变。
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
CORE_LIBS = ("avformat", "avcodec", "avutil", "swscale", "swresample")


def run(*args):
    """工具失败立即终止，避免把未完整收集或未签名的库发布到项目。"""
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def brew_prefix(formula=None):
    env = dict(os.environ, HOMEBREW_NO_AUTO_UPDATE="1")
    args = ["brew", "--prefix"]
    root = Path(subprocess.run(args, env=env, check=True, text=True,
                               capture_output=True).stdout.strip()).resolve()
    # 已安装的 opt 链接可直接读取，避免公式查询触发 Homebrew 的联网更新。
    return (root / "opt" / formula).resolve(strict=True) if formula else root


def install_name(binary):
    lines = run("otool", "-D", str(binary)).splitlines()
    return lines[1].strip() if len(lines) > 1 else ""


def dependencies(binary):
    own_id = install_name(binary)
    result = []
    for line in run("otool", "-L", str(binary)).splitlines()[1:]:
        match = re.match(r"\s+(.+?)\s+\(compatibility version", line)
        if match and match.group(1) != own_id:
            result.append(match.group(1))
    return result


def system_library(ref):
    return ref.startswith(("/usr/lib/", "/System/Library/"))


def rpaths(binary):
    result = []
    lines = run("otool", "-l", str(binary)).splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "cmd LC_RPATH":
            for following in lines[i + 1:i + 4]:
                match = re.match(r"\s+path (.+?) \(offset", following)
                if match:
                    result.append(match.group(1))
    return result


def resolve_dependency(binary, ref, search_dirs):
    """严格解析每条引用；未知 @rpath 或失效绝对路径不能静默忽略。"""
    if ref.startswith("@loader_path/"):
        candidates = [binary.parent / ref[len("@loader_path/"):]]
    elif ref.startswith("@rpath/"):
        name = ref[len("@rpath/"):]
        local_dirs = []
        for value in rpaths(binary):
            if value.startswith("@loader_path/"):
                local_dirs.append(binary.parent / value[len("@loader_path/"):])
            elif value == "@loader_path":
                local_dirs.append(binary.parent)
            elif not value.startswith("@"):
                local_dirs.append(Path(value))
        candidates = [directory / name for directory in local_dirs + search_dirs]
    elif ref.startswith("@"):
        raise RuntimeError(f"无法解析动态库引用：{binary}: {ref}")
    else:
        candidates = [Path(ref)]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(f"动态库依赖缺失：{binary}: {ref}")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def publish_bundle(args, prefix, output, roots, visited, edges):
    output.parent.mkdir(parents=True, exist_ok=True)
    # 完整收集、修复和签名成功后才替换目标目录，任何错误都保留旧依赖。
    with tempfile.TemporaryDirectory(prefix=".ffmpeg-bundle-", dir=output.parent) as temporary:
        stage = Path(temporary) / "bundle"
        lib_dir = stage / "lib"
        inc_dir = stage / "include"
        lib_dir.mkdir(parents=True)
        inc_dir.mkdir()
        for binary, name in visited.items():
            target = lib_dir / name
            shutil.copy2(binary, target)
            target.chmod(0o755)
            run("install_name_tool", "-id", f"@loader_path/{name}", str(target))
            for old_ref, dependency in edges[binary].items():
                run("install_name_tool", "-change", old_ref,
                    f"@loader_path/{visited[dependency]}", str(target))
            run("codesign", "--force", "-s", "-", str(target))
        for root in roots:
            name = visited[root]
            short = re.sub(r"\.\d+.*\.dylib$", ".dylib", name)
            major = re.sub(r"(\.\d+)\.\d+(\.\d+)*\.dylib$", r"\1.dylib", name)
            for alias in {short, major} - {name}:
                (lib_dir / alias).symlink_to(name)
        for headers in sorted((prefix / "include").glob("lib*")):
            if headers.is_dir():
                shutil.copytree(headers, inc_dir / headers.name)
        if not (inc_dir / "libavformat" / "avformat.h").is_file():
            raise RuntimeError("FFmpeg 头文件缺失")

        for name in visited.values():
            target = lib_dir / name
            for ref in dependencies(target):
                if system_library(ref):
                    continue
                if not ref.startswith("@loader_path/") or not (lib_dir / ref[13:]).is_file():
                    raise RuntimeError(f"打包后依赖不完整：{name}: {ref}")
            run("codesign", "--verify", "--strict", str(target))
        patch_info = []
        if args.patch_file:
            patch_path = args.patch_file.resolve(strict=True)
            try:
                recorded_path = str(patch_path.relative_to(PROJECT_ROOT))
            except ValueError:
                recorded_path = patch_path.name
            patch_info.append({"file": recorded_path, "sha256": sha256(patch_path)})
        metadata = {
            "ffmpegVersion": "4.4.6",
            "sourceRef": args.source_ref,
            "basePrefix": str(prefix),
            "patches": patch_info,
            "avformatSha256": sha256(lib_dir / visited[roots[0]]),
            "libraries": sorted(visited.values()),
        }
        (stage / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n")
        previous = Path(temporary) / "previous"
        if output.exists():
            output.rename(previous)
        try:
            stage.rename(output)
        except BaseException:
            if previous.exists():
                previous.rename(output)
            raise
    print(f"已更新 {output}；全部依赖在包内解析且签名有效")


def bundle(args):
    prefix = args.ffmpeg_prefix.resolve() if args.ffmpeg_prefix else brew_prefix("ffmpeg@4")
    output = args.output_dir.absolute()
    if output.is_symlink():
        raise RuntimeError("输出目录不能是符号链接")
    if args.patch_file and not args.avformat_library:
        raise RuntimeError("--patch-file 必须同时指定 --avformat-library")
    metadata_path = output / "build-info.json"
    if not args.avformat_library and not args.allow_unpatched and metadata_path.is_file():
        if json.loads(metadata_path.read_text()).get("patches"):
            raise RuntimeError("目标已含 FLV 补丁。请使用 scripts/ffmpeg/build_ffmpeg_macos.sh；"
                               "显式传 --allow-unpatched 才能恢复原始 Homebrew 库。")

    roots = [(prefix / "lib" / f"lib{name}.dylib").resolve(strict=True) for name in CORE_LIBS]
    override = None
    original_avformat = roots[0]
    if original_avformat.name != "libavformat.58.76.100.dylib":
        raise RuntimeError("需要 FFmpeg 4.4.6，不能混用不同版本的头文件和核心库")
    if args.avformat_library:
        override = args.avformat_library.resolve(strict=True)
        if override.name != original_avformat.name:
            raise RuntimeError(f"补丁库版本不匹配：{override.name} != {original_avformat.name}")
        roots[0] = override

    search_dirs = [prefix / "lib", brew_prefix() / "lib"]
    visited = {}
    edges = {}
    names = {}
    def collect(binary):
        binary = binary.resolve(strict=True)
        if binary in visited:
            return
        if binary.name in names and names[binary.name] != binary:
            raise RuntimeError(f"依赖文件名冲突：{binary.name}")
        names[binary.name] = binary
        visited[binary] = binary.name
        edges[binary] = {}
        for ref in dependencies(binary):
            if system_library(ref):
                continue
            dependency = resolve_dependency(binary, ref, search_dirs)
            if override and dependency == original_avformat:
                dependency = override
            edges[binary][ref] = dependency
            collect(dependency)

    for root in roots:
        collect(root)
    print(f"已解析 {len(visited)} 个动态库")

    publish_bundle(args, prefix, output, roots, visited, edges)



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg-prefix", type=Path, help="与项目 ABI 一致的 FFmpeg 4.4.6 安装目录")
    parser.add_argument("--avformat-library", type=Path, help="用补丁构建的 libavformat 版本库")
    parser.add_argument("--patch-file", type=Path, help="记录补丁来源和 SHA-256")
    parser.add_argument("--source-ref", default="Homebrew ffmpeg@4")
    parser.add_argument("--output-dir", type=Path,
                        default=PROJECT_ROOT / "third_party" / "ffmpeg-macos")
    parser.add_argument("--allow-unpatched", action="store_true", help="允许覆盖现有补丁版本")
    args = parser.parse_args()
    try:
        bundle(args)
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"FFmpeg 打包失败：{error}\n")


if __name__ == "__main__":
    main()
