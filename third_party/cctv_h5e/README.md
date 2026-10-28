# CCTV H5E NAL processing

Source: https://github.com/letr007/CCTVVideoDownloader/blob/main/src/core/src/crypto/cctv_h5e_decrypt.hpp

Retrieved on 2026-10-28 through the source website. The copy retains the NAL
transforms and Session, omitting TS rewriting, C bindings and the optional CLI.
No Qt, WASM or external executable is used.

License: GPL-3.0-only, Copyright (C) 2025 Letr. See LICENSE. This is GPL code
linked into FluxPlayer; distributing the combined program requires compliance
with GPL-3.0, including its corresponding-source requirements. This notice
does not relicense unrelated third-party files.

The FluxPlayer adapter is in src/utils/H264NalTransformer.cpp. Classic live mode
must be explicitly enabled only for recognized CCTV protected H.264 sources.
Ordinary HLS and generic browser EME/DRM are not treated as CCTV H5E.

Classic no-type25 live streams use a different tail rule from legacy H5E VOD:
the final complete 8-byte TEA cell is processed even when fewer than 80 bytes
remain. FluxPlayer implements that live-only rule in the adapter and compacts
EPBs in one linear pass. The vendored VOD/session routines remain unchanged.
