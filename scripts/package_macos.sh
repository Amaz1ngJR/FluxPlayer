#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT/build"
APP_NAME="FluxPlayer"
APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
DMG_OUT="$BUILD_DIR/$APP_NAME.dmg"

# 1. 构建
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release -j$(sysctl -n hw.logicalcpu)

# 2. 创建 .app 结构
rm -rf "$APP_BUNDLE"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources"

cp "$BUILD_DIR/$APP_NAME" "$APP_BUNDLE/Contents/MacOS/"
cp -r "$BUILD_DIR/shaders" "$APP_BUNDLE/Contents/MacOS/"
cp -r "$BUILD_DIR/fonts"   "$APP_BUNDLE/Contents/MacOS/"

# 复制 FFmpeg dylib
find "$BUILD_DIR" -maxdepth 1 -name "*.dylib" -exec cp {} "$APP_BUNDLE/Contents/MacOS/" \;

# 3. 图标：将 source/pic.png 转为 .icns
ICONSET="$BUILD_DIR/AppIcon.iconset"
mkdir -p "$ICONSET"
for size in 16 32 64 128 256 512; do
    sips -z $size $size "$ROOT/source/pic.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    sips -z $((size*2)) $((size*2)) "$ROOT/source/pic.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP_BUNDLE/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET"

# 4. Info.plist
cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>             <string>FluxPlayer</string>
    <key>CFBundleExecutable</key>       <string>FluxPlayer</string>
    <key>CFBundleIdentifier</key>       <string>com.fluxplayer.app</string>
    <key>CFBundleVersion</key>          <string>0.1.0</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>CFBundleIconFile</key>         <string>AppIcon</string>
    <key>CFBundlePackageType</key>      <string>APPL</string>
    <key>NSHighResolutionCapable</key>  <true/>
</dict>
</plist>
EOF

# 5. 打 DMG
rm -f "$DMG_OUT"
hdiutil create -volname "$APP_NAME" -srcfolder "$APP_BUNDLE" \
    -ov -format UDZO "$DMG_OUT"

echo "Done: $DMG_OUT"
