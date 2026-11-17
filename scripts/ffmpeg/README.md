# Shared FFmpeg baseline

- Source commit: `5a1f107b4c78318f318897900acad1b6c6aa3f9d` (Windows existing 7.1.3 branch).
- GitHub commit archive SHA-256: `971722f1268e84ac6b759ae0c0f3de2d6164ed40bd845e4769c89fd77b9b7f23`.
- Apply `patches/series` in order on **both platforms**. Never apply the historical 4.4 patch to 7.
- Windows avformat-61.dll now includes the complete series; the original matching headers/import libraries and other DLLs are retained. See windows-x64/build-info.json for hashes, configuration and validation scope.

## Patch contracts

1. `ffmpeg-7.1-legacy-hevc.patch`: unchanged existing Windows codec-id 12 reader.
2. `ffmpeg-7.1-private-flv.patch`: opt-in `-flvflags legacy_hevc` writer (hvcC, NALs, signed CTS, EOS) and raw private Opus codec-id 13, 48 kHz mono/stereo. No OpusHead/pre-skip/multichannel mapping. Default HEVC writer remains upstream Enhanced FLV. Applications must request the flag to emit legacy HEVC; existing recording code is not modified to force it.
3. `ffmpeg-7.1-enhanced-flv-fixes.patch`: minimal regression fixes, not the old Enhanced parser. Skip unknown FourCC/EOS before creating tracks, bound headers and feed av1C configOBUs on keyframes (including queued configuration updates).
4. `ffmpeg-7.1-hls-fmp4-seek.patch`: recreate the MOV child demuxer when seeking an unencrypted EXT-X-MAP playlist. Resetting AVIO offsets while retaining old fragment/sample indexes can otherwise produce invalid H.264 NAL sizes. Encrypted playlists retain the upstream path. Both the Windows DLL and checked-in macOS artifact include this fix.

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

The macOS verifier also generates an unencrypted HLS/fMP4 fixture and checks 12 decoded frames at each of three seek targets against direct MP4 decoding, including timestamps and pixel hashes. The previous macOS library reproduced invalid NAL sizes and returned no frames on this fixture; the rebuilt library passes. FLV regression, dependency closure/ad-hoc signatures, CMake Release and xmake builds were also verified. This does not cover encrypted HLS, GUI repeated seeks, real network sources or hardware-backend acceptance. CMake and xmake both redeploy changed FFmpeg runtime libraries even without player source changes.

Prebuilt artifacts live under `third_party/ffmpeg/macos-arm64/` and `third_party/ffmpeg/windows-x64/`, with separate generated headers. The Windows verifier defaults to the latter. Update any explicit old `FFMPEG_ROOT` overrides in local CMake caches; automatic defaults and packaging use the new paths. This layout move preserves library/header bytes and symlink targets.

This delivers artifacts using installed Homebrew dependency versions, not a byte-for-byte reproducible external dependency toolchain. `build-info.json` records the configure flags, patch hashes and bundled library hashes. Preserve upstream notices/GPLv3 distribution and corresponding-source obligations, including bundled external dependencies.

## Windows

Use a clean checkout at the same commit and the original fully configured Windows build environment. `build_ffmpeg_windows.sh --source-dir ... --build-dir ...` applies the same series, rebuilds only avformat-61.dll and never installs it. Original headers/import libraries/codec DLLs must stay matched. For an already patched source tree, rerun `make -C <build-dir> -j4 libavformat/avformat-61.dll` directly.

```powershell
python scripts/ffmpeg/verify_ffmpeg_windows.py --self-test --cc C:/msys64/mingw64/bin/gcc.exe --avformat-dll build/ffmpeg-windows-objects/libavformat/avformat-61.dll
```

The verifier supports MSVC (`--cc cl`) and MinGW. It loads core DLLs beside an isolated executable and checks exact header versions, preserves the baseline container/protocol list, generates short encoded streams, and verifies default Enhanced / explicit legacy HEVC writers, mono/stereo private Opus, multichannel rejection, AV1/VP9, AV1 configuration-only startup, extra tags and truncated headers. Without `--self-test`, it additionally compares the externally supplied legacy/Enhanced HEVC fixtures. Generation requires the matching project's libx265/libopus/libaom/libvpx encoders. It does not use a system FFmpeg CLI or overwrite project DLLs. Hardware backends, GUI, seek/reconnect, live network sources and long-running playback still require scenario-specific acceptance.

The fMP4 seek patch was additionally validated using the failing source's init section and short fragments served by a local HTTP server. The unpatched DLL reproduced the exact invalid-NAL errors; the patched DLL decoded 12 frames after the requested target with timestamps and pixel MD5s identical to direct MP4 decoding. These private fixtures are not committed, and this check is separate from the self-test above. C++ HLS reopening alone did not resolve this fragment-index bug. After updating the dependency, close any running player before rebuilding with xmake so its post-build step can replace the runtime DLL in build/bin.

## Verification

The dated macOS regression summary is maintained in [the hardware decoding document](../../docs/硬件加速解码技术文档.md#ffmpeg-封装兼容补丁), rather than duplicated here. Library hashes and patch provenance are retained in the platform's `build-info.json`.

No regression claim for GUI playback, seek/reconnect, real remote HTTP-FLV/RTMP services, recording/download/merge integration, long clips or Windows runtime. CLI fixtures are not substitutes for those checks.
