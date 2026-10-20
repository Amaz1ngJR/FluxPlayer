#include "SRPixelConvert.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define FLUX_SR_SSE2 1
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define FLUX_SR_NEON 1
#endif
namespace FluxPlayer {
void packSuperResolutionRgbReference(const float* red, const float* green, const float* blue,
                                     uint8_t* destination, size_t pixels) {
    for (size_t x = 0; x < pixels; ++x) {
        const float channels[] = {red[x], green[x], blue[x]};
        for (size_t c = 0; c < 3; ++c) {
            if (!std::isfinite(channels[c])) throw std::runtime_error("Non-finite model output");
            destination[x * 3 + c] = static_cast<uint8_t>(std::lround(std::clamp(channels[c], 0.f, 1.f) * 255.f));
        }
    }
}
namespace {
bool nativeLroundAddsHalf() {
    // Some Windows CRTs implement lroundf by adding 0.5f before truncation.
    // Preserve that observable behavior too, including values just below a tie.
    static const bool result = [] {
        volatile float belowHalf = std::nextafter(.5f, 0.f);
        return std::lround(belowHalf) == 1;
    }();
    return result;
}
uint8_t quantize(float value, bool addsHalf) {
    if (!std::isfinite(value)) throw std::runtime_error("Non-finite model output");
    const float scaled = std::clamp(value, 0.f, 1.f) * 255.f;
    if (addsHalf) return static_cast<uint8_t>(static_cast<int>(scaled + .5f));
    const int integer = static_cast<int>(scaled);
    // Adding 0.5f before truncation can round values just below a tie upward.
    // Compare the exact fractional part to keep the original lroundf behavior.
    return static_cast<uint8_t>(integer + (scaled - static_cast<float>(integer) >= .5f));
}
#ifdef FLUX_SR_SSE2
__m128i quantize4(__m128 value, bool addsHalf) {
    const __m128i exponent = _mm_and_si128(_mm_castps_si128(value), _mm_set1_epi32(0x7f800000));
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000))))
        throw std::runtime_error("Non-finite model output");
    const __m128 scaled = _mm_mul_ps(_mm_min_ps(_mm_max_ps(value, _mm_setzero_ps()), _mm_set1_ps(1.f)), _mm_set1_ps(255.f));
    if (addsHalf) return _mm_cvttps_epi32(_mm_add_ps(scaled, _mm_set1_ps(.5f)));
    const __m128i integer = _mm_cvttps_epi32(scaled);
    const __m128 fractional = _mm_sub_ps(scaled, _mm_cvtepi32_ps(integer));
    const __m128i roundUp = _mm_and_si128(_mm_castps_si128(_mm_cmpge_ps(fractional, _mm_set1_ps(.5f))), _mm_set1_epi32(1));
    return _mm_add_epi32(integer, roundUp);
}
#endif
#ifdef FLUX_SR_NEON
uint32x4_t quantizeNeon4(float32x4_t value, bool addsHalf) {
    const auto exponent=vandq_u32(vreinterpretq_u32_f32(value),vdupq_n_u32(0x7f800000));
    if(vmaxvq_u32(vceqq_u32(exponent,vdupq_n_u32(0x7f800000))))
        throw std::runtime_error("Non-finite model output");
    const auto scaled=vmulq_n_f32(vminq_f32(vmaxq_f32(value,vdupq_n_f32(0.f)),vdupq_n_f32(1.f)),255.f);
    if(addsHalf)return vcvtq_u32_f32(vaddq_f32(scaled,vdupq_n_f32(.5f)));
    const auto integer=vcvtq_u32_f32(scaled);
    const auto fractional=vsubq_f32(scaled,vcvtq_f32_u32(integer));
    return vaddq_u32(integer,vandq_u32(vcgeq_f32(fractional,vdupq_n_f32(.5f)),vdupq_n_u32(1)));
}
uint8x8_t quantizeNeon8(const float* value, bool addsHalf) {
    return vmovn_u16(vcombine_u16(vmovn_u32(quantizeNeon4(vld1q_f32(value),addsHalf)),
                                  vmovn_u32(quantizeNeon4(vld1q_f32(value+4),addsHalf))));
}
#endif
}
void packSuperResolutionRgb(const float* red, const float* green, const float* blue,
                            uint8_t* destination, size_t pixels) {
    size_t x = 0;
    const bool addsHalf = nativeLroundAddsHalf();
#ifdef FLUX_SR_NEON
    // 与标量路径相同的裁剪、有限值检查和取整，交织写入8个RGB像素。
    for (; x + 8 <= pixels; x += 8) {
        uint8x8x3_t rgb;
        rgb.val[0]=quantizeNeon8(red+x,addsHalf);
        rgb.val[1]=quantizeNeon8(green+x,addsHalf);
        rgb.val[2]=quantizeNeon8(blue+x,addsHalf);
        vst3_u8(destination+x*3,rgb);
    }
#endif
#ifdef FLUX_SR_SSE2
    for (; x + 4 <= pixels; x += 4) {
        const auto r = quantize4(_mm_loadu_ps(red + x), addsHalf);
        const auto g = quantize4(_mm_loadu_ps(green + x), addsHalf);
        const auto b = quantize4(_mm_loadu_ps(blue + x), addsHalf);
        const auto rgb = _mm_or_si128(r, _mm_or_si128(_mm_slli_epi32(g, 8), _mm_slli_epi32(b, 16)));
        uint32_t words[4];
        _mm_storeu_si128(reinterpret_cast<__m128i*>(words), rgb);
        for (size_t i = 0; i < 4; ++i) std::memcpy(destination + (x + i) * 3, words + i, 3);
    }
#endif
    for (; x < pixels; ++x) {
        destination[x * 3] = quantize(red[x], addsHalf);
        destination[x * 3 + 1] = quantize(green[x], addsHalf);
        destination[x * 3 + 2] = quantize(blue[x], addsHalf);
    }
}
}
