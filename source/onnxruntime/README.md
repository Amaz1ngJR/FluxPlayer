# ONNX Runtime — offline SR only

- Upstream: https://github.com/microsoft/onnxruntime/releases/tag/v1.23.2
- Asset: `onnxruntime-osx-arm64-1.23.2.tgz` (GitHub release asset ID 308474683).
- Downloaded via GitHub release assets API; not a third-party mirror.
- Archive SHA-256: `b4d513ab2b26f088c66891dbbc1408166708773d7cc4163de7bdca0e9bbb7856`.
- `macos-arm64/libonnxruntime.1.23.2.dylib` SHA-256:
  `d306d2bc768540766c7ed8a1e0ff05d2870c77a934ebeee4a7bafa1b732ef299`.
- Architecture: Mach-O arm64. Version 1.23.2; MIT license and upstream
  ThirdPartyNotices.txt are included. C ABI headers are in third_party/onnxruntime.
- `otool -L` shows Apple frameworks and system libraries only; no Homebrew paths.
- Loaded by absolute resource path only when an offline export starts. No link-time
  dependency; missing/incompatible library fails the task, not player startup.
- CMake/xmake source-resource deployment includes this directory and its licenses.
- The exact macOS arm64 dylib above is distributed in Git (an explicit `.gitignore`
  exception), so a fresh checkout does not depend on a developer's ignored file.
  CMake install deploys it under `resources/onnxruntime/`; the macOS packaging
  script copies it into `Contents/Resources/onnxruntime/`, checks its SHA-256,
  and includes it in the bundled dynamic-library dependency checks.
- No Windows CUDA, Intel Mac or Linux runtime is bundled. Do not advertise support
  for those targets based on this arm64 artifact.
- CoreML initialization on this Mac reports an OptimizationHints warning and partial
  CPU node placement. CoreML EP selection is not evidence of all-GPU/ANE execution.
- Release package signing/notarization and clean-machine installation remain unverified.

## Windows x64

- Official NuGet: [Microsoft.ML.OnnxRuntime.DirectML 1.23.0](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.23.0),
  native `win-x64` DLLs; ORT API 23 matches the vendored 1.23.2 C header.
  DirectML has no 1.23.2 NuGet release; keep its runtime version explicit.
- Package SHA-256: `a33ec2382b3c440bab74042a135733bb6e5085f293b908d3997688a58fe307e7`.
- Dependency: [Microsoft.AI.DirectML 1.15.4](https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4),
  release `bin/x64-win/DirectML.dll` (not the debug DLL).
- Dependency package SHA-256: `4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9`.
- Per-DLL hashes are in `windows-x64/SHA256SUMS`; ORT uses the MIT license above,
  its Windows notices and the DirectML license/notices are included beside the DLLs.
- Windows 10/11 x64; Microsoft Visual C++ v14 x64 Redistributable is required by
  the official ORT build. If missing, install it from
  [Microsoft](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
  Loader errors include the Windows error code and this prerequisite.
- Auto mode tries DirectML on the DXGI high-performance adapter (DirectX 12), with memory patterns disabled
  and sequential execution. Initialization failure retries a new CPU session
  before creating output. CPU mode bypasses DirectML initialization entirely.
  Selection does not guarantee that every operator runs on the GPU.
- The actual GPU name and FP32 execution are reported. Original model weights,
  tile/halo geometry and full-frame processing remain unchanged.
- Both UI and CLI resolve `onnxruntime/windows-x64/onnxruntime.dll` via the same
  resource resolver; DLLs load by absolute path. DirectML is explicitly loaded
  from that directory. No ORT link dependency is added to player startup.
- xmake and CMake deploy the DLLs and licenses; Windows packaging verifies hashes
  and includes these resources through the existing recursive installer rule.
