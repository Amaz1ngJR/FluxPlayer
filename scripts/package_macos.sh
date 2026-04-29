#!/bin/bash
# macOS 打包脚本：生成 FluxPlayer.app 并打包为 FluxPlayer.dmg
#
# 依赖（均为 macOS 系统自带，无需额外安装）：
#   - cmake        构建系统
#   - sips         图片缩放（系统工具）
#   - iconutil     .iconset -> .icns 转换（系统工具）
#   - hdiutil      创建 .dmg 磁盘镜像（系统工具）
#
# 图标优先级：
#   1. source/pic.icns（已有则直接使用）
#   2. source/pic.png（自动转换为 .icns）
#
# 用法：
#   ./scripts/package_macos.sh
#
# 输出：
#   build/FluxPlayer.dmg

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT/build"
APP_NAME="FluxPlayer"
# 从 CMakeLists.txt 读取版本号，发版只需改那一处
VERSION=$(grep -m1 'project(FluxPlayer VERSION' "$ROOT/CMakeLists.txt" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
DMG_OUT="$BUILD_DIR/$APP_NAME-$VERSION.dmg"

# ── 1. CMake 构建（Release 模式）──────────────────────────────────────────────
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release -j$(sysctl -n hw.logicalcpu)

# ── 2. 创建 .app bundle 目录结构 ──────────────────────────────────────────────
# macOS .app 规范：
#   Contents/MacOS/   存放可执行文件和运行时依赖
#   Contents/Resources/ 存放图标等资源
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"

# 复制可执行文件
cp "$BUILD_DIR/$APP_NAME" "$APP_BUNDLE/Contents/MacOS/"

# 复制运行时资源（shader 和字体）
cp -r "$BUILD_DIR/shaders" "$APP_BUNDLE/Contents/MacOS/"
cp -r "$BUILD_DIR/fonts"   "$APP_BUNDLE/Contents/MacOS/"

# 复制 FFmpeg dylib（rpath 设为 @executable_path，dylib 需与可执行文件同目录）
find "$BUILD_DIR" -maxdepth 1 -name "*.dylib" -exec cp {} "$APP_BUNDLE/Contents/MacOS/" \;

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
    for size in 16 32 64 128 256 512; do
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
rm -f "$DMG_OUT"
hdiutil create -volname "$APP_NAME" -srcfolder "$APP_BUNDLE" \
    -ov -format UDZO "$DMG_OUT"

echo "Done: $DMG_OUT"
