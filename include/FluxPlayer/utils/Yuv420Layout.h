#pragma once

namespace FluxPlayer {

constexpr int chroma420Extent(int extent) {
    return extent / 2 + extent % 2;
}

// NV12 每个色度样本由交错的 U、V 两字节组成。
template <typename Visit>
bool visitYuv420Planes(int width, int height, bool nv12, Visit visit) {
    const int cw = chroma420Extent(width), ch = chroma420Extent(height);
    return visit(0, height, width) && visit(1, ch, nv12 ? 2 * cw : cw) &&
           (nv12 || visit(2, ch, cw));
}

} // namespace FluxPlayer
