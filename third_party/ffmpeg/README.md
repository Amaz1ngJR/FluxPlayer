# FFmpeg prebuilt dependencies

```text
ffmpeg/
├── macos-arm64/     # macOS arm64: include/, lib/, build-info.json
└── windows-x64/     # Windows x64: include/, lib/ import libraries, bin/ DLLs
```

Both platforms use source commit `5a1f107b4c78318f318897900acad1b6c6aa3f9d`.
Build/patch instructions and validation limits are in [scripts/ffmpeg/README.md](../../scripts/ffmpeg/README.md).
Do not share or overwrite generated headers between platforms. This directory consolidation
does not change binary contents or apply new patches to the Windows DLLs.

CMake and xmake select the platform subdirectory by default; `FFMPEG_ROOT` can point
to a complete candidate prefix for isolated CMake validation. Explicit overrides
must point to a platform subdirectory or complete candidate prefix, not this parent directory.
Deployment output layouts next to the executable are unchanged.

Build provenance is in each platform's `build-info.json`. Both platform artifacts
include the four patches in `scripts/ffmpeg/patches/series`, including HLS/fMP4 seek.
Windows retains the original matching codec/util/filter/scale/resample DLLs,
headers and import libraries; only avformat was rebuilt there.
Keep upstream licensing and corresponding-source obligations when distributing the libraries.
