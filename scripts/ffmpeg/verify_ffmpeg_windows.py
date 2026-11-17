#!/usr/bin/env python3
"""编译 C 探针，在隔离目录中验证 Windows FFmpeg 的 FLV 读写与解码。

支持 x64 MSVC 或 MinGW GCC；仅使用 Python 标准库。
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
    if "--capabilities" not in command or result.returncode:
        print(output, end="" if output.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise RuntimeError(f"进程失败，退出码 {result.returncode}；参见上方输出")
    return output


def verify(candidate, legacy, enhanced, project, compiler, work_dir, timeout, self_test=False):
    candidate, project = [
        Path(path).resolve(strict=True) for path in (candidate, project)
    ]
    for fixture in (() if self_test else (legacy, enhanced)):
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
        # 使用 ASCII 相对路径，避免 C main 的 Windows ANSI argv 损坏中文素材路径。
        if not self_test:
            shutil.copy2(legacy, stage / "legacy.flv")
            shutil.copy2(enhanced, stage / "enhanced.flv")
        executable = stage / "verify_ffmpeg_windows.exe"
        if Path(compiler).stem.lower() in ("cl", "clang-cl"):
            command = [compiler, "/nologo", "/MD", "/W4", "/utf-8",
                       f"/I{project / 'include'}", PROBE,
                       f"/Fo{stage / 'probe.obj'}", f"/Fe{executable}", "/link",
                       f"/LIBPATH:{project / 'lib'}", "avformat.lib", "avcodec.lib", "avutil.lib"]
        else:
            command = [compiler, "-std=c11", "-O2", "-Wall", f"-I{project / 'include'}",
                       PROBE, f"-L{project / 'lib'}", "-lavformat", "-lavcodec", "-lavutil",
                       "-o", executable]
        run(command, stage, dict(os.environ), timeout)
        env = dict(os.environ)
        # 不让其他 FFmpeg 安装通过 PATH 填补依赖；系统运行库仍由 Windows 加载。
        system = Path(os.environ["SystemRoot"])
        env["PATH"] = os.pathsep.join(map(str, (stage, system / "System32", system)))
        baseline = run([executable, "--capabilities"], stage, env, timeout)
        shutil.copy2(candidate, stage / CORE_DLLS[0])
        actual = run([executable, "--capabilities"], stage, env, timeout)
        def capabilities(output):
            return {line.strip() for line in output.splitlines()
                    if line.startswith(("demuxer:", "muxer:", "input:", "output:"))}
        missing = capabilities(baseline) - capabilities(actual)
        if missing:
            raise ValueError(f"候选库减少了容器/协议能力: {sorted(missing)}")
        print(f"PASS: preserved {len(capabilities(baseline))} container/protocol capabilities")
        if not self_test:
            run([executable, "legacy.flv", "enhanced.flv"], stage, env, timeout)
        run([executable, "--contracts"], stage, env, timeout)
        # The writer must retain upstream Enhanced output by default, and emit
        # codec-id 12 only when explicitly requested. Compare real decoded pixels/PTS.
        from verify_ffmpeg_macos import read_flv, write_flv, without_av1_sequence
        for filename, enhanced_output in (("contract-enhanced.flv", True),
                                           ("contract-legacy.flv", False)):
            _, tags = read_flv(stage / filename)
            video = [payload for kind, _, payload in tags if kind == 9]
            if not video or any(bool(payload[0] & 0x80) != enhanced_output for payload in video):
                raise ValueError(f"HEVC writer contract failed: {filename}")
        run([executable, "contract-legacy.flv", "contract-enhanced.flv"], stage, env, timeout)
        header, tags = read_flv(stage / "contract-enhanced.flv")
        sequence = next(tag for tag in tags if tag[0] == 9 and tag[2][0] & 15 == 0)
        write_flv(stage / "extra-tags.flv", header,
                  [sequence, (9, 0, b"\x90zzzzunknown"), *tags, (9, 1000, b"\x92hvc1")])
        run([executable, "contract-legacy.flv", "extra-tags.flv"], stage, env, timeout)
        for payload in (b"\x91hv", b"\x91hvc1\x00\x00"):
            write_flv(stage / "truncated.flv", header, [sequence, (9, 1, payload)])
            run([executable, "--reject", "truncated.flv"], stage, env, timeout)
        for codec in ("av1", "vp9"):
            filename = f"contract-{codec}.flv"
            run([executable, "--compare", codec, filename, filename], stage, env, timeout)
        header, tags = read_flv(stage / "contract-av1.flv")
        stripped, removed = [], 0
        for kind, timestamp, payload in tags:
            if kind == 9 and payload[0] & 15 == 1:
                body, count = without_av1_sequence(payload[5:])
                payload = payload[:5] + body
                removed += count
            stripped.append((kind, timestamp, payload))
        if not removed:
            raise ValueError("AV1 test did not remove any in-band sequence headers")
        write_flv(stage / "av1-config-only.flv", header, stripped)
        run([executable, "--compare", "av1", "contract-av1.flv", "av1-config-only.flv"], stage, env, timeout)
    print("PASS: 候选 Windows DLL 验证通过；项目 DLL 未修改。")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--avformat-dll", type=Path, required=True, help="待验证的补丁 avformat-61.dll")
    parser.add_argument("--legacy", type=Path, default=ROOT / "build/ffmpeg-flv-tests/legacy-hevc.flv")
    parser.add_argument("--enhanced", type=Path, default=ROOT / "build/ffmpeg-flv-tests/enhanced-hevc.flv",
                        help="与 legacy 由同一码流生成的 Enhanced FLV")
    parser.add_argument("--ffmpeg-dir", type=Path, default=ROOT / "third_party/ffmpeg/windows-x64",
                        help="项目 FFmpeg 目录（含 include/lib/bin）")
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/ffmpeg-windows-tests")
    parser.add_argument("--cc", default="cl", help="x64 MSVC cl.exe 或 MinGW gcc.exe 路径")
    parser.add_argument("--timeout", type=int, default=120, help="每个进程的超时秒数")
    parser.add_argument("--self-test", action="store_true", help="生成短素材进行完整回归，无需外部 HEVC 测试文件")
    args = parser.parse_args(argv)
    if sys.platform != "win32":
        parser.error("此入口必须在 Windows 上运行；本机源码回归不能替代 Windows DLL 验证")
    if args.timeout <= 0:
        parser.error("--timeout 必须大于 0")
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error("未找到编译器；请使用 x64 VS Developer Command Prompt 或 --cc 指定 MinGW gcc.exe")
    try:
        verify(args.avformat_dll, args.legacy, args.enhanced, args.ffmpeg_dir,
               compiler, args.work_dir.resolve(), args.timeout, args.self_test)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
