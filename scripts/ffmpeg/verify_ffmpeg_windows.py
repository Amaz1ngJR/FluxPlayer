#!/usr/bin/env python3
"""编译小型 C 探针，在隔离目录中验证 Windows 候选 DLL 的 legacy HEVC FLV 解码。

在 x64 VS Developer Command Prompt 中运行；仅使用 Python 标准库。
不调用系统 ffmpeg/ffprobe，不修改项目 DLL，不负责构建 FFmpeg。
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PROBE = Path(__file__).with_suffix(".c")
CORE_DLLS = ("avformat-61.dll", "avcodec-61.dll", "avutil-59.dll")


def check_x64_dll(path):
    """在启动进程前拒绝非 x64 PE DLL，避免误用其他平台/架构产物。"""
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError(f"不是 PE DLL: {path}")
        offset = struct.unpack_from("<I", header, 60)[0]
        stream.seek(offset)
        pe = stream.read(24)
    if (len(pe) != 24 or pe[:4] != b"PE\0\0"
            or struct.unpack_from("<H", pe, 4)[0] != 0x8664
            or not struct.unpack_from("<H", pe, 22)[0] & 0x2000):
        raise ValueError(f"需要 x86_64 Windows DLL: {path}")


def run(command, cwd, env, timeout):
    print("+", subprocess.list2cmdline([str(arg) for arg in command]), flush=True)
    result = subprocess.run([str(arg) for arg in command], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    # cl 使用本地代码页；探针路径和 FFmpeg 日志可能包含 UTF-8。
    output = result.stdout.decode("utf-8", errors="replace")
    print(output, end="" if output.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise RuntimeError(f"进程失败，退出码 {result.returncode}；参见上方输出")


def verify(candidate, legacy, enhanced, project, compiler, work_dir, timeout):
    candidate, legacy, enhanced, project = [
        Path(path).resolve(strict=True) for path in (candidate, legacy, enhanced, project)
    ]
    for fixture in (legacy, enhanced):
        if not fixture.is_file():
            raise ValueError(f"测试素材不是文件: {fixture}")
    for name in CORE_DLLS:
        check_x64_dll(candidate if name == CORE_DLLS[0] else project / "bin" / name)
    for name in ("avformat", "avcodec", "avutil"):
        if not (project / "lib" / f"{name}.lib").is_file():
            raise ValueError(f"缺少项目导入库: {name}.lib")
    if not (project / "include" / "libavformat" / "avformat.h").is_file():
        raise ValueError("缺少项目 FFmpeg 头文件")
    work_dir.mkdir(parents=True, exist_ok=True)
    # 每次建立独占目录；编译对象和 DLL 副本均在其中，不污染源码或运行目录。
    with tempfile.TemporaryDirectory(prefix="windows-flv-", dir=work_dir) as temporary:
        stage = Path(temporary)
        for dll in (project / "bin").iterdir():
            if dll.is_file() and dll.suffix.lower() == ".dll":
                shutil.copy2(dll, stage / dll.name)
        shutil.copy2(candidate, stage / CORE_DLLS[0])
        # 使用 ASCII 相对路径，避免 C main 的 Windows ANSI argv 损坏中文素材路径。
        shutil.copy2(legacy, stage / "legacy.flv")
        shutil.copy2(enhanced, stage / "enhanced.flv")
        executable = stage / "verify_ffmpeg_windows.exe"
        run([compiler, "/nologo", "/MD", "/W4", "/utf-8",
             f"/I{project / 'include'}", PROBE,
             f"/Fo{stage / 'probe.obj'}", f"/Fe{executable}", "/link",
             f"/LIBPATH:{project / 'lib'}", "avformat.lib", "avcodec.lib", "avutil.lib"],
            stage, dict(os.environ), timeout)
        env = dict(os.environ)
        # 不让其他 FFmpeg 安装通过 PATH 填补依赖；系统运行库仍由 Windows 加载。
        system = Path(os.environ["SystemRoot"])
        env["PATH"] = os.pathsep.join(map(str, (stage, system / "System32", system)))
        run([executable, "legacy.flv", "enhanced.flv"], stage, env, timeout)
    print("PASS: 候选 Windows DLL 验证通过；项目 DLL 未修改。")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--avformat-dll", type=Path, required=True, help="待验证的补丁 avformat-61.dll")
    parser.add_argument("--legacy", type=Path, default=ROOT / "build/ffmpeg-flv-tests/legacy-hevc.flv")
    parser.add_argument("--enhanced", type=Path, default=ROOT / "build/ffmpeg-flv-tests/enhanced-hevc.flv",
                        help="与 legacy 由同一码流生成的 Enhanced FLV")
    parser.add_argument("--ffmpeg-dir", type=Path, default=ROOT / "third_party/ffmpeg",
                        help="项目 FFmpeg 目录（含 include/lib/bin）")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/ffmpeg-windows-tests")
    parser.add_argument("--cc", default="cl", help="x64 MSVC cl.exe 路径")
    parser.add_argument("--timeout", type=int, default=120, help="每个进程的超时秒数")
    args = parser.parse_args(argv)
    if sys.platform != "win32":
        parser.error("此入口必须在 Windows 上运行；本机源码回归不能替代 Windows DLL 验证")
    if args.timeout <= 0:
        parser.error("--timeout 必须大于 0")
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error("未找到 cl.exe，请在 x64 VS Developer Command Prompt 中运行")
    try:
        verify(args.avformat_dll, args.legacy, args.enhanced, args.ffmpeg_dir,
               compiler, args.work_dir.resolve(), args.timeout)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
