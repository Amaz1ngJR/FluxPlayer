#!/bin/bash
# Build the Windows FFmpeg source baseline for macOS; publish candidates only.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMMIT=5a1f107b4c78318f318897900acad1b6c6aa3f9d
ARCHIVE_SHA=971722f1268e84ac6b759ae0c0f3de2d6164ed40bd845e4769c89fd77b9b7f23
WORK="$ROOT/build/ffmpeg7-macos"
ARCHIVE=""
REPO=""
JOBS="$(getconf _NPROCESSORS_ONLN)"
usage() {
    echo "Usage: bash scripts/ffmpeg/build_ffmpeg_macos.sh [--source-archive FILE | --source-dir GIT_REPO] [--jobs N]"
    echo "Offline source required; candidate: build/ffmpeg7-macos/bundle (no project replacement)."
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-archive|--source-dir|--jobs)
            [ "$#" -ge 2 ] || { usage >&2; exit 1; }
            case "$1" in
                --source-archive) ARCHIVE="$2" ;;
                --source-dir) REPO="$2" ;;
                --jobs) JOBS="$2" ;;
            esac
            shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done
[ "$(uname -s)" = Darwin ] || { echo "macOS required" >&2; exit 1; }
case "$JOBS" in *[!0-9]*|'') usage >&2; exit 1 ;; esac
[ "$JOBS" -gt 0 ] || exit 1
[ -z "$REPO" ] || [ -z "$ARCHIVE" ] || { usage >&2; exit 1; }
mkdir -p "$WORK"
STRIP=1
if [ -n "$REPO" ]; then
    [ "$(git -C "$REPO" rev-parse "$COMMIT^{commit}")" = "$COMMIT" ] || exit 1
    ARCHIVE="$WORK/source.tar"
    git -C "$REPO" archive "$COMMIT" --output="$ARCHIVE"
    STRIP=0
else
    ARCHIVE="${ARCHIVE:-$ROOT/build/ffmpeg-windows-source.tar.gz}"
    printf '%s  %s\n' "$ARCHIVE_SHA" "$ARCHIVE" | shasum -a 256 -c -
fi
ARCHIVE="$(cd "$(dirname "$ARCHIVE")" && pwd)/$(basename "$ARCHIVE")"
SERIES="$SCRIPT_DIR/patches/series"
PATCHES=()
while IFS= read -r name || [ -n "$name" ]; do
    case "$name" in ''|\#*) continue ;; esac
    case "$name" in */*|*..*) echo "Invalid patch name: $name" >&2; exit 1 ;; esac
    PATCHES+=("$SCRIPT_DIR/patches/$name")
done < "$SERIES"
[ "${#PATCHES[@]}" -gt 0 ] || exit 1
FINGERPRINT="$(shasum -a 256 "$ARCHIVE" "$SCRIPT_DIR/build_ffmpeg_macos.sh" "$SERIES" "${PATCHES[@]}")"
SOURCE="$WORK/source"
OBJECTS="$WORK/objects"
PREFIX="$WORK/install"
if [ ! -f "$WORK/fingerprint" ] || [ "$(cat "$WORK/fingerprint")" != "$FINGERPRINT" ]; then
    rm -rf "$SOURCE" "$OBJECTS" "$PREFIX"
    mkdir -p "$SOURCE" "$OBJECTS"
    tar -xf "$ARCHIVE" --strip-components="$STRIP" -C "$SOURCE"
    [ "$(cat "$SOURCE/RELEASE")" = 7.1.3 ] || { echo "Unexpected source" >&2; exit 1; }
    # Avoid version.sh discovering the enclosing player's Git repository.
    printf '%s\n' 'n7.1.3-43-g5a1f107b4c' > "$SOURCE/VERSION"
    for patch_file in "${PATCHES[@]}"; do
        (cd "$SOURCE" && patch --batch --fuzz=0 -p1 -i "$patch_file")
    done
    printf '%s\n' "$FINGERPRINT" > "$WORK/fingerprint"
fi
export HOMEBREW_NO_AUTO_UPDATE=1
BREW_PREFIX="$(brew --prefix)"
SDK="$(xcrun --show-sdk-path)"
mkdir -p "$WORK/pkgconfig"
printf 'Name: libXML\nDescription: macOS SDK libxml2\nVersion: %s\nLibs: %s/usr/lib/libxml2.tbd\nCflags: -I%s/usr/include/libxml2\n' \
    "$(/usr/bin/xml2-config --version)" "$SDK" "$SDK" > "$WORK/pkgconfig/libxml-2.0.pc"
export PKG_CONFIG_PATH="$WORK/pkgconfig:$BREW_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
# Reuse installed dependencies, never silently download or install packages.
pkg-config --exists x264 x265 opus vpx zimg gnutls srt librist libbluray libxml-2.0 libzmq dav1d aom libass soxr
cd "$OBJECTS"
"$SOURCE/configure" --prefix="$PREFIX" --install-name-dir="$PREFIX/lib" \
    --cc=clang --enable-shared --disable-static --disable-debug --disable-doc \
    --disable-autodetect --disable-ffplay --enable-gpl --enable-version3 \
    --enable-videotoolbox --enable-audiotoolbox --enable-avfoundation --enable-network \
    --enable-indev=avfoundation --enable-indev=lavfi --enable-outdev=audiotoolbox \
    --enable-libx264 --enable-libx265 --enable-libopus --enable-libvpx \
    --enable-libdav1d --enable-libaom --enable-libzimg --enable-libass --enable-libsoxr \
    --enable-libmp3lame --enable-libvorbis --enable-libtheora --enable-libwebp \
    --enable-libspeex --enable-libopenjpeg --enable-libopencore-amrnb --enable-libopencore-amrwb \
    --enable-libsnappy --enable-librubberband --enable-libvidstab --enable-libtesseract \
    --enable-libfontconfig --enable-libfreetype \
    --enable-librav1e --enable-libxvid \
    --enable-gnutls --enable-libsrt --enable-librist --enable-libbluray \
    --enable-libxml2 --enable-libzmq --enable-lzma --enable-zlib --enable-bzlib \
    --extra-cflags="-I$BREW_PREFIX/include" --extra-ldflags="-L$BREW_PREFIX/lib" \
    --extra-version="FluxPlayer-$COMMIT"
make -j "$JOBS"
make install
python3 - "$PREFIX" "$COMMIT" "$SERIES" <<'PY'
import hashlib, json, pathlib, sys
prefix, commit, series = pathlib.Path(sys.argv[1]), sys.argv[2], pathlib.Path(sys.argv[3])
patches = {name: hashlib.sha256((series.parent / name).read_bytes()).hexdigest()
           for name in series.read_text().splitlines() if name and not name.startswith('#')}
(prefix / 'flux-build.json').write_text(json.dumps({'sourceRef': commit, 'patches': patches}) + '\n')
PY
python3 "$SCRIPT_DIR/bundle_ffmpeg_macos.py" --ffmpeg-prefix "$PREFIX" \
    --output-dir "$WORK/bundle" --source-ref "$COMMIT"
echo "Candidate ready: $WORK/bundle. Test before replacing third_party/ffmpeg/macos-arm64."
