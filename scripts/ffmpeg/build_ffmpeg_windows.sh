#!/bin/bash
# 在原 Windows FFmpeg 构建环境中重编 libavformat；不自动替换项目 DLL。
# 必须使用匹配项目版本、已完整配置的源码/构建目录，保留原依赖和协议能力。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PATCH="$ROOT/scripts/ffmpeg/patches/ffmpeg-7.1-legacy-hevc.patch"
COMMIT=5a1f107b4c78318f318897900acad1b6c6aa3f9d
SOURCE=""
OBJECTS=""
JOBS=4
usage() {
    echo "用法：bash scripts/ffmpeg/build_ffmpeg_windows.sh --source-dir <原 FFmpeg Git 源码> --build-dir <已配置的构建目录> [--jobs N]"
    echo "会修改指定源码的 libavformat/flv.h 和 flvdec.c；只构建 avformat-61.dll，不安装或覆盖项目库。"
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-dir|--build-dir|--jobs)
            [ "$#" -ge 2 ] || { usage >&2; exit 1; }
            case "$1" in
                --source-dir) SOURCE="$2" ;;
                --build-dir) OBJECTS="$2" ;;
                --jobs) JOBS="$2" ;;
            esac
            shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done
[ -n "$SOURCE" ] && [ -n "$OBJECTS" ] || { usage >&2; exit 1; }
case "$JOBS" in *[!0-9]*|'') echo "--jobs 必须是正整数" >&2; exit 1 ;; esac
[ "$JOBS" -gt 0 ] || exit 1
SOURCE="$(cd "$SOURCE" && pwd)"
OBJECTS="$(cd "$OBJECTS" && pwd)"
[ "$(git -C "$SOURCE" rev-parse HEAD)" = "$COMMIT" ] || {
    echo "源码必须匹配项目 n7.1.3-43-g5a1f107b4c，不接受其他 7.x 版本" >&2; exit 1;
}
# 不自动重新 configure，也不通过 --disable-everything 缩减项目运行库能力。
# 使用原构建环境的依赖、工具链、配置与生成的 import libraries。
[ -f "$OBJECTS/ffbuild/config.mak" ] && [ -f "$OBJECTS/config.h" ] || {
    echo "需要原发行构建中已配置的目录和完整依赖环境，不能仅提供 DLL/头文件" >&2; exit 1;
}
grep -q '^CONFIG_SHARED=yes' "$OBJECTS/ffbuild/config.mak"
grep -q '^SLIBSUF=\.dll$' "$OBJECTS/ffbuild/config.mak"
grep -q '^ARCH_X86_64=yes' "$OBJECTS/ffbuild/config.mak"
# source_path 由 FFmpeg configure 写入，确保构建确实消费即将打补丁的源码。
CONFIG_SOURCE="$(sed -n 's/^SRC_PATH=//p' "$OBJECTS/ffbuild/config.mak")"
[ -n "$CONFIG_SOURCE" ] && [ "$(cd "$OBJECTS" && cd "$CONFIG_SOURCE" && pwd)" = "$SOURCE" ] || {
    echo "构建目录 SRC_PATH 不匹配 --source-dir" >&2; exit 1;
}
# 只在干净源码上首次应用，不覆盖并行修改；重复构建可直接运行下面的 make 命令。
git -C "$SOURCE" diff --quiet && git -C "$SOURCE" diff --cached --quiet || {
    echo "源码有未提交改动，请使用干净的独立工作树；已应用补丁时直接运行 make" >&2; exit 1;
}
git -C "$SOURCE" apply --check "$PATCH"
git -C "$SOURCE" apply "$PATCH"
make -C "$OBJECTS" -j "$JOBS" libavformat/avformat-61.dll
[ -f "$OBJECTS/libavformat/avformat-61.dll" ]
echo "已生成 $OBJECTS/libavformat/avformat-61.dll"
echo "尚未覆盖 third_party/ffmpeg/bin/avformat-61.dll。请先用项目其余原 DLL 验证解码与依赖加载。"
echo "补丁仅扩展 FLV 读取；不修改 Enhanced FLV 写入，不支持私有 Opus codec-id 13。"
