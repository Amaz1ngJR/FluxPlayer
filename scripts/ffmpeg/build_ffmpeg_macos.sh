#!/bin/bash
# 为项目构建 FLV 兼容补丁；只替换同 ABI 的 libavformat，保留完整解码能力。
# 用法：bash scripts/ffmpeg/build_ffmpeg_macos.sh [--source-dir <FFmpeg Git 仓库>]
# 默认下载并校验官方 4.4.6 源码，也可用 --source-archive 指定官方 tar.xz。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
WORK_DIR="$ROOT/build/ffmpeg-macos-patched"
PATCH_FILE="$SCRIPT_DIR/patches/ffmpeg-4.4.6-flv-compat.patch"
VERSION="4.4.6"
SOURCE_SHA256="2290461f467c08ab801731ed412d8e724a5511d6c33173654bd9c1d2e25d0617"
SOURCE_COMMIT="44b04492bfc83215e136f2a68783bff71d328692"
SOURCE_REPO=""
SOURCE_ARCHIVE=""
JOBS="$(getconf _NPROCESSORS_ONLN)"

usage() {
    echo "用法：bash scripts/ffmpeg/build_ffmpeg_macos.sh [--source-dir <Git 仓库> | --source-archive <官方 tar.xz>] [--jobs <数量>]"
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-dir|--source-archive|--jobs)
            [ "$#" -ge 2 ] || { usage >&2; exit 1; }
            case "$1" in
                --source-dir) SOURCE_REPO="$2" ;;
                --source-archive) SOURCE_ARCHIVE="$2" ;;
                --jobs) JOBS="$2" ;;
            esac
            shift 2
            ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done
[ "$(uname -s)" = "Darwin" ] || { echo "此脚本仅适用于 macOS" >&2; exit 1; }
case "$JOBS" in *[!0-9]*|'') echo "--jobs 必须为正整数" >&2; exit 1 ;; esac
[ "$JOBS" -gt 0 ] || { echo "--jobs 必须大于 0" >&2; exit 1; }
[ -z "$SOURCE_REPO" ] || [ -z "$SOURCE_ARCHIVE" ] || { usage >&2; exit 1; }

export HOMEBREW_NO_AUTO_UPDATE=1
BREW_PREFIX="$(brew --prefix)"
FFMPEG_PREFIX="$BREW_PREFIX/opt/ffmpeg@4"
[ -f "$FFMPEG_PREFIX/lib/libavformat.58.76.100.dylib" ] || {
    echo "需要 FFmpeg 4.4.6 (libavformat 58.76.100)，不能混用不同 ABI 的库" >&2
    exit 1
}
mkdir -p "$WORK_DIR/pkgconfig"
SDK_PATH="$(xcrun --show-sdk-path)"
XML_VERSION="$(/usr/bin/xml2-config --version)"
# 原项目库使用系统 libxml2；沿用 SDK 的头文件与导入库，避免额外引入 ICU。
printf 'prefix=%s\nName: libXML\nDescription: macOS SDK libxml2\nVersion: %s\nLibs: "%s/usr/lib/libxml2.tbd"\nCflags: -I"%s/usr/include/libxml2"\n' \
    "$SDK_PATH" "$XML_VERSION" "$SDK_PATH" "$SDK_PATH" > "$WORK_DIR/pkgconfig/libxml-2.0.pc"
export PKG_CONFIG_PATH="$WORK_DIR/pkgconfig:$BREW_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# 这些传输/封装依赖都来自现有 Homebrew FFmpeg，不能在重编后减少协议支持。
pkg-config --exists gnutls srt librist libbluray libxml-2.0 libzmq liblzma
mkdir -p "$WORK_DIR"
STRIP_COMPONENTS=1
if [ -n "$SOURCE_REPO" ]; then
    COMMIT="$(git -C "$SOURCE_REPO" rev-parse "n$VERSION^{commit}")"
    [ "$COMMIT" = "$SOURCE_COMMIT" ] || { echo "源码标签不是预期的官方 n$VERSION" >&2; exit 1; }
    SOURCE_ARCHIVE="$WORK_DIR/ffmpeg-$VERSION.tar"
    # 导出固定标签，忽略调用者仓库的未提交补丁及编译产物，不修改原仓库。
    git -C "$SOURCE_REPO" archive "n$VERSION" --output="$SOURCE_ARCHIVE"
    STRIP_COMPONENTS=0
else
    if [ -z "$SOURCE_ARCHIVE" ]; then
        SOURCE_ARCHIVE="$WORK_DIR/ffmpeg-$VERSION.tar.xz"
        if [ ! -f "$SOURCE_ARCHIVE" ]; then
            curl --fail --location --proto '=https' --tlsv1.2 \
                "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o "$SOURCE_ARCHIVE.part"
            mv "$SOURCE_ARCHIVE.part" "$SOURCE_ARCHIVE"
        fi
    fi
    ACTUAL_SHA="$(shasum -a 256 "$SOURCE_ARCHIVE" | awk '{print $1}')"
    [ "$ACTUAL_SHA" = "$SOURCE_SHA256" ] || { echo "FFmpeg 源码校验失败" >&2; exit 1; }
fi
SOURCE_ARCHIVE="$(cd "$(dirname "$SOURCE_ARCHIVE")" && pwd)/$(basename "$SOURCE_ARCHIVE")"
FINGERPRINT="$(shasum -a 256 "$SOURCE_ARCHIVE" "$PATCH_FILE")"
STAMP="$WORK_DIR/source-fingerprint.txt"
SOURCE_DIR="$WORK_DIR/source"
OBJECT_DIR="$WORK_DIR/objects"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$FINGERPRINT" ]; then
    # 仅清理该脚本独占的生成目录，补丁或源码变化时禁止复用旧配置对象。
    rm -rf "$SOURCE_DIR" "$OBJECT_DIR"
    mkdir -p "$SOURCE_DIR" "$OBJECT_DIR"
    tar -xf "$SOURCE_ARCHIVE" --strip-components="$STRIP_COMPONENTS" -C "$SOURCE_DIR"
    [ "$(cat "$SOURCE_DIR/RELEASE")" = "$VERSION" ] || { echo "源码版本错误" >&2; exit 1; }
    (cd "$SOURCE_DIR" && patch --batch --fuzz=0 -p1 -i "$PATCH_FILE")
    printf '%s\n' "$FINGERPRINT" > "$STAMP"
fi

cd "$OBJECT_DIR"
"$SOURCE_DIR/configure" \
    --prefix="$WORK_DIR/install" \
    --install-name-dir="$FFMPEG_PREFIX/lib" \
    --extra-cflags="-I$BREW_PREFIX/include" --extra-ldflags="-L$BREW_PREFIX/lib" \
    --cc=clang --enable-shared --disable-static --disable-debug --disable-doc \
    --disable-programs --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-autodetect --enable-network --enable-gpl --enable-version3 \
    --enable-videotoolbox --enable-audiotoolbox \
    --enable-gnutls --enable-libsrt --enable-librist --enable-libbluray \
    --enable-libxml2 --enable-libzmq --enable-lzma --enable-zlib --enable-bzlib
make -j "$JOBS" libavformat/libavformat.dylib
make install-libavformat-shared

# 编译时依赖相同版本的核心库，发布时继续使用 Homebrew 完整 codec 库。
# 所有库通过收集脚本重写为 @loader_path，运行时不依赖 Homebrew。
python3 "$SCRIPT_DIR/bundle_ffmpeg_macos.py" \
    --ffmpeg-prefix "$FFMPEG_PREFIX" \
    --avformat-library "$WORK_DIR/install/lib/libavformat.58.76.100.dylib" \
    --patch-file "$PATCH_FILE" --source-ref "n$VERSION"
echo "FFmpeg FLV 补丁已写入 third_party/ffmpeg-macos；请重新链接播放器。"
