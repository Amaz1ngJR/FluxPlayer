#!/bin/bash
# macOS 打包脚本：生成 FluxPlayer.app 并打包为 FluxPlayer.dmg
#
# 依赖：
#   - cmake        构建系统（需预先安装）
#   - sips         图片缩放（系统工具）
#   - iconutil     .iconset -> .icns 转换（系统工具）
#   - hdiutil      创建 .dmg 磁盘镜像（系统工具）
#   - otool        检查动态库引用（系统工具）
#
# 图标优先级：
#   1. source/pic.icns（已有则直接使用）
#   2. source/pic.png（自动转换为 .icns）
#
# 用法：
#   ./scripts/package_macos.sh
#
# 输出：
#   dist/FluxPlayer-<版本号>.dmg

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
# CMake 中间文件固定放在 build/cmake，最终运行产物统一放在 build/bin。
# 输出目录由 CMakeLists.txt 配置；这里分别保存路径，避免打包脚本依赖旧布局。
BUILD_DIR="$ROOT/build/cmake"
BIN_DIR="$ROOT/build/bin"
APP_NAME="FluxPlayer"
# 从 CMakeLists.txt 读取版本号，发版只需改那一处
VERSION=$(grep -m1 'project(FluxPlayer VERSION' "$ROOT/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
APP_BUNDLE="$BIN_DIR/$APP_NAME.app"
DMG_OUT="$ROOT/dist/$APP_NAME-$VERSION.dmg"
FFMPEG_LIB_DIR="$ROOT/third_party/ffmpeg-macos/lib"
YTDLP="$ROOT/third_party/yt-dlp/yt-dlp_macos"

for required in "$FFMPEG_LIB_DIR/libavformat.dylib" "$YTDLP" \
    "$ROOT/source/pic2.png" "$ROOT/source/video/video_01.mp4" \
    "$ROOT/source/UI/skins/cyberpunk-neon/cyberpunk-neon.lua"; do
    if [ ! -f "$required" ]; then
        echo "Missing packaging input: $required" >&2
        exit 1
    fi
done

# ── 1. CMake 构建（Release 模式）──────────────────────────────────────────────
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release --parallel

# ── 2. 创建 .app bundle 目录结构 ──────────────────────────────────────────────
# macOS .app 规范：
#   Contents/MacOS/   存放可执行文件和运行时依赖
#   Contents/Resources/ 存放图标等资源
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"

# 复制 build/bin 中的共享可执行文件
cp "$BIN_DIR/$APP_NAME" "$APP_BUNDLE/Contents/MacOS/"

# shaders/fonts 必须与可执行文件同目录：
# GLRenderer 用 getExeDir()+"/shaders/..." 定位着色器，字体按可执行文件同级 fonts/
# 加载，二者都依赖「与 exe 同级」而非 Resources，因此放进 Contents/MacOS/
cp -r "$BIN_DIR/shaders" "$APP_BUNDLE/Contents/MacOS/"
cp -r "$BIN_DIR/fonts"   "$APP_BUNDLE/Contents/MacOS/"

# pic2.png：纯音频封面兜底图，Player 通过 Config::getResourcePath("pic2.png") 加载，
# macOS 优先解析 ../Resources/，故直接放在 Resources 根下
cp "$ROOT/source/pic2.png" "$APP_BUNDLE/Contents/Resources/pic2.png"

# video_01.mp4：硬件解码测速样片，HardwareInfo 通过 Config::getResourcePath("video/video_01.mp4") 加载
mkdir -p "$APP_BUNDLE/Contents/Resources/video"
cp "$ROOT/source/video/video_01.mp4" "$APP_BUNDLE/Contents/Resources/video/video_01.mp4"

# 皮肤包：Config::getResourcePath("skins") 在 macOS 优先解析到 ../Resources/skins，
# 单文件 Lua 皮肤是唯一运行时入口；保留目录内 SVG 预览和设计稿。
mkdir -p "$APP_BUNDLE/Contents/Resources/skins/cyberpunk-neon"
cp -R "$ROOT/source/UI/skins/cyberpunk-neon/." \
      "$APP_BUNDLE/Contents/Resources/skins/cyberpunk-neon/"
mkdir -p "$APP_BUNDLE/Contents/Resources/skins/minimal-lua"
cp -R "$ROOT/source/UI/skins/minimal-lua/." \
      "$APP_BUNDLE/Contents/Resources/skins/minimal-lua/"

# 从当前声明的 FFmpeg 目录复制完整依赖闭包，避免共享 build/bin 中的旧库混入发布包。
# dylib 内部使用 @loader_path，版本别名为相对符号链接，两者都要求库与 exe 同级。
cp -R "$FFMPEG_LIB_DIR/." "$APP_BUNDLE/Contents/MacOS/"

# 发布前检查每个 Mach-O 引用：系统库允许外链，其他依赖必须已在包内。
for binary in "$APP_BUNDLE/Contents/MacOS/$APP_NAME" "$APP_BUNDLE/Contents/MacOS/"*.dylib; do
    [ -e "$binary" ] || { echo "Broken bundled library: $binary" >&2; exit 1; }
    otool -L "$binary" | awk 'NR > 1 { print $1 }' | while IFS= read -r dependency; do
        case "$dependency" in
            @loader_path/*|@executable_path/*)
                [ -e "$APP_BUNDLE/Contents/MacOS/${dependency#*/}" ] || {
                    echo "Missing dependency for $binary: $dependency" >&2
                    exit 1
                }
                ;;
            /System/*|/usr/lib/*) ;;
            *) echo "Unbundled dependency for $binary: $dependency" >&2; exit 1 ;;
        esac
    done
done

# yt-dlp：网页流提取依赖，必须与可执行文件同级（getYtDlpPath 优先在 exe 同级查找），
# 否则装到目标机后运行时报「找不到 dlp」。保留可执行权限。
cp "$YTDLP" "$APP_BUNDLE/Contents/MacOS/yt-dlp_macos"
chmod +x "$APP_BUNDLE/Contents/MacOS/yt-dlp_macos"

# ── 3. 图标处理 ───────────────────────────────────────────────────────────────
ICNS_OUT="$APP_BUNDLE/Contents/Resources/AppIcon.icns"
if [ -f "$ROOT/source/pic.icns" ]; then
    # 已有 .icns，直接使用
    cp "$ROOT/source/pic.icns" "$ICNS_OUT"
else
    # 从 source/pic.png 生成 .icns
    # iconutil 要求 iconset 目录包含各尺寸 PNG，命名格式固定
    ICONSET="$BUILD_DIR/AppIcon.iconset"
    mkdir -p "$ICONSET"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$ROOT/source/pic.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
        sips -z $((size*2)) $((size*2)) "$ROOT/source/pic.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$ICNS_OUT"
    rm -rf "$ICONSET"
fi

# ── 4. Info.plist（告知 macOS 如何启动 .app）─────────────────────────────────
cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>             <string>FluxPlayer</string>
    <key>CFBundleExecutable</key>       <string>FluxPlayer</string>
    <key>CFBundleIdentifier</key>       <string>com.fluxplayer.app</string>
    <key>CFBundleVersion</key>          <string>$VERSION</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleIconFile</key>         <string>AppIcon</string>
    <key>CFBundlePackageType</key>      <string>APPL</string>
    <key>NSHighResolutionCapable</key>  <true/>
</dict>
</plist>
EOF

# ── 5. 打包为 .dmg 磁盘镜像 ──────────────────────────────────────────────────
# UDZO = zlib 压缩格式，体积小，兼容性好
mkdir -p "$ROOT/dist"
rm -f "$DMG_OUT"
hdiutil create -volname "$APP_NAME" -srcfolder "$APP_BUNDLE" \
    -ov -format UDZO "$DMG_OUT"

echo "Done: $DMG_OUT"
