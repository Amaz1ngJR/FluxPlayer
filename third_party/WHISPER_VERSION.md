# whisper.cpp

Vendored source: https://github.com/ggml-org/whisper.cpp

Pinned upstream revision: `aba02defd6bb6ff5de63721c21e3456645c153e2` (1.9.5-dev).

License: MIT; see `whisper.cpp/LICENSE`.

Trimmed for FluxPlayer: retains library sources/headers, CMake files, CPU/BLAS/Metal backends, authors and license. Removes examples, bindings, tests, samples, media, CI, standalone tooling and unused accelerator backends. Some source files are retained because upstream CMake references them even when their target is not built. CMake builds Whisper/GGML statically with upstream examples and tests disabled. This snapshot is intended for use as a FluxPlayer subdirectory, not as the full standalone upstream project.

The built-in model is tracked separately at `source/models/whisper/ggml-base-q8_0.bin`. Optional small weights are not bundled; see that directory's README for the download link.
