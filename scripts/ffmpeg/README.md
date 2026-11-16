# Shared FFmpeg baseline

- Source commit: `5a1f107b4c78318f318897900acad1b6c6aa3f9d` (Windows existing 7.1.3 branch).
- GitHub commit archive SHA-256: `971722f1268e84ac6b759ae0c0f3de2d6164ed40bd845e4769c89fd77b9b7f23`.
- Apply `patches/series` in order on **both platforms**. Never apply the historical 4.4 patch to 7.
- Existing Windows DLL remains unchanged; the shared recipe is ahead of that artifact until Windows rebuild/validation.

## Patch contracts

1. `ffmpeg-7.1-legacy-hevc.patch`: unchanged existing Windows codec-id 12 reader.
2. `ffmpeg-7.1-private-flv.patch`: opt-in `-flvflags legacy_hevc` writer (hvcC, NALs, signed CTS, EOS) and raw private Opus codec-id 13, 48 kHz mono/stereo. No OpusHead/pre-skip/multichannel mapping. Default HEVC writer remains upstream Enhanced FLV. Applications must request the flag to emit legacy HEVC; existing recording code is not modified to force it.
3. `ffmpeg-7.1-enhanced-flv-fixes.patch`: minimal regression fixes, not the old Enhanced parser. Skip unknown FourCC/EOS before creating tracks, bound headers and feed av1C configOBUs on keyframes (including queued configuration updates).

## macOS

Use installed Homebrew dependencies; scripts do not install/download them. Native arm64 is validated; Intel/universal builds are not claimed. The build script enumerates external dependencies explicitly and enables VideoToolbox/AudioToolbox plus avfilter/libzimg.

```sh
bash scripts/ffmpeg/build_ffmpeg_macos.sh --source-dir /local/ffmpeg --jobs 8
# Alternative: verified GitHub commit archive (not a 7.1.3 release tarball)
# --source-archive /local/FFmpeg-5a1f107b4c78318f318897900acad1b6c6aa3f9d.tar.gz
python3 scripts/ffmpeg/verify_ffmpeg_macos.py \
  --ffmpeg-prefix "$PWD/build/ffmpeg7-macos/install" \
  --library-dir "$PWD/build/ffmpeg7-macos/bundle/lib"
cmake -S . -B build/ffmpeg7-app -DCMAKE_BUILD_TYPE=Release \
  -DFFMPEG_ROOT="$PWD/build/ffmpeg7-macos/bundle" \
  -DFLUXPLAYER_RUNTIME_OUTPUT_DIR="$PWD/build/ffmpeg7-app/bin"
cmake --build build/ffmpeg7-app -j8
```

No formal dependencies are overwritten by building. Bundle collection validates the installed source marker, ABI group and build provenance, resolves all non-system dependencies, rewrites loader paths and ad-hoc signs them. After validation, explicitly invoke the bundler with `--output-dir "$PWD/third_party/ffmpeg/macos-arm64"`. Keep an old-library backup outside the source deployment and relink the entire player. Do not mix 4.x and 7.x libraries. Runtime/build caches may contain obsolete libraries; package only the declared dependency directory, not the whole build/bin.

Prebuilt artifacts live under `third_party/ffmpeg/macos-arm64/` and `third_party/ffmpeg/windows-x64/`, with separate generated headers. The Windows verifier defaults to the latter. Update any explicit old `FFMPEG_ROOT` overrides in local CMake caches; automatic defaults and packaging use the new paths. This layout move preserves library/header bytes and symlink targets.

This delivers artifacts using installed Homebrew dependency versions, not a byte-for-byte reproducible external dependency toolchain. `build-info.json` records the configure flags, patch hashes and bundled library hashes. Preserve upstream notices/GPLv3 distribution and corresponding-source obligations, including bundled external dependencies.

## Windows

Use a clean checkout at the same commit and the original fully configured Windows build environment. `build_ffmpeg_windows.sh --source-dir ... --build-dir ...` applies the same series, rebuilds only avformat-61.dll and never installs it. Original headers/import libraries/codec DLLs must stay matched. Validate candidate DLL with `verify_ffmpeg_windows.py`, then separately test new writers and Opus on Windows. Existing Windows verification compares HEVC decoding; it is not full coverage of the new series or hardware backends.

## Verification

The dated macOS regression summary is maintained in [the hardware decoding document](../../docs/硬件加速解码技术文档.md#ffmpeg-封装兼容补丁), rather than duplicated here. Library hashes and patch provenance are retained in the platform's `build-info.json`.

No regression claim for GUI playback, seek/reconnect, real remote HTTP-FLV/RTMP services, recording/download/merge integration, long clips or Windows runtime. CLI fixtures are not substitutes for those checks.
