#pragma once
#include <cstddef>
#include <cstdint>
namespace FluxPlayer {
/// Pack planar FP32 model output to RGB24, exactly matching lround(clamp(x)*255).
/// Reject NaN/Inf; never change precision or rounding semantics.
void packSuperResolutionRgb(const float* red, const float* green, const float* blue,
                            uint8_t* destination, size_t pixels);
/// Original conversion used only by the opt-in quality/performance comparison.
void packSuperResolutionRgbReference(const float* red, const float* green, const float* blue,
                                     uint8_t* destination, size_t pixels);
}
