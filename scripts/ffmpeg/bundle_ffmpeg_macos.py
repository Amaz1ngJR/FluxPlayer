#!/usr/bin/env python3
"""
收集 macOS FFmpeg 动态库依赖，修复安装路径并签名。
收集共同 FFmpeg 7 基线构建的完整库组，默认仅发布候选目录。
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
CORE_LIBS = {
    "avformat": 61,
    "avcodec": 61,
    "avutil": 59,
    "swscale": 8,
    "swresample": 5,
    "avfilter": 10,
    "avdevice": 61,
}
SOURCE_REF = "5a1f107b4c78318f318897900acad1b6c6aa3f9d"


def run(*args):
    """工具失败立即终止，避免把未完整收集或未签名的库发布到项目。"""
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def brew_prefix():
    env = dict(os.environ, HOMEBREW_NO_AUTO_UPDATE="1")
    args = ["brew", "--prefix"]
    return Path(subprocess.run(args, env=env, check=True, text=True,
                               capture_output=True).stdout.strip()).resolve()


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
        series = PROJECT_ROOT / "scripts/ffmpeg/patches/series"
        for line in series.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            if Path(line).name != line or ".." in line:
                raise RuntimeError("Invalid patch series")
            patch_path = series.parent / line
            patch_info.append({"file": str(patch_path.relative_to(PROJECT_ROOT)), "sha256": sha256(patch_path)})
        if not patch_info:
            raise RuntimeError("Empty patch series")
        provenance = json.loads((prefix / "flux-build.json").read_text())
        if provenance != {"sourceRef": SOURCE_REF,
                          "patches": {Path(p["file"]).name: p["sha256"] for p in patch_info}}:
            raise RuntimeError("Installed build provenance does not match current patch series")
        metadata = {
            "ffmpegVersion": "7.1.3",
            "sourceRef": args.source_ref,
            "patches": patch_info,
            "avformatSha256": sha256(lib_dir / visited[roots[0]]),
            "libraries": sorted(visited.values()),
            "headersSha256": sha256(prefix / "include/libavutil/ffversion.h"),
        }
        configuration = run(str(prefix / "bin/ffmpeg"), "-version").splitlines()[2]
        # Preserve build flags without committing developer-specific absolute paths.
        metadata["configuration"] = configuration.replace(str(prefix), "${FFMPEG_PREFIX}").replace(
            str(PROJECT_ROOT), "${PROJECT_ROOT}")
        metadata["sha256"] = {name: sha256(lib_dir / name) for name in visited.values()}
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
    prefix = args.ffmpeg_prefix.resolve(strict=True)
    output = args.output_dir.absolute()
    if output.is_symlink():
        raise RuntimeError("输出目录不能是符号链接")
    if args.source_ref != SOURCE_REF:
        raise RuntimeError("Source must match the Windows FFmpeg commit")
    version = (prefix / "include/libavutil/ffversion.h").read_text()
    if SOURCE_REF not in version:
        raise RuntimeError("Prefix does not identify the pinned FFmpeg source")

    roots = [(prefix / "lib" / f"lib{name}.dylib").resolve(strict=True) for name in CORE_LIBS]
    for (name, major), binary in zip(CORE_LIBS.items(), roots):
        if not binary.name.startswith(f"lib{name}.{major}.") or binary.parent != (prefix / "lib").resolve():
            raise RuntimeError(f"Mixed FFmpeg ABI or prefix: {binary}")

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
            if re.match(r"lib(avformat|avcodec|avutil|avfilter|avdevice|swscale|swresample)\.", dependency.name) and dependency.parent != (prefix / "lib").resolve():
                raise RuntimeError(f"FFmpeg core dependency escaped prefix: {dependency}")
            edges[binary][ref] = dependency
            collect(dependency)

    for root in roots:
        collect(root)
    print(f"已解析 {len(visited)} 个动态库")

    publish_bundle(args, prefix, output, roots, visited, edges)
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg-prefix", type=Path, required=True, help="共同 FFmpeg 7 基线完整安装目录")
    parser.add_argument("--source-ref", default=SOURCE_REF)
    parser.add_argument("--output-dir", type=Path,
                        default=PROJECT_ROOT / "build/ffmpeg7-macos/bundle")
    args = parser.parse_args()
    try:
        bundle(args)
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"FFmpeg 打包失败：{error}\n")


if __name__ == "__main__":
    main()
