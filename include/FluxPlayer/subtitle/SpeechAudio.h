#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace FluxPlayer {
// Shared amplitude policy for chunking and inference (16 kHz mono float PCM).
// Energy alone cannot distinguish speech from background noise.
struct SpeechAudio {
    static constexpr size_t block = 320;
    static constexpr double quietEnergy = 1e-7; // -70 dBFS RMS power

    static double energy(const float* samples, size_t count) {
        if (!count) return 0;
        double sum = 0, squares = 0;
        for (size_t i = 0; i < count; ++i) {
            const double value = samples[i];
            sum += value;
            squares += value * value;
        }
        const double mean = sum / count;
        return std::max(0.0, squares / count - mean * mean);
    }

    static std::vector<float> prepare(const float* samples, size_t count) {
        double activeEnergy = 0, mean = 0;
        size_t activeSamples = 0, consecutive = 0, longest = 0;
        for (size_t offset = 0; offset < count; offset += block) {
            const size_t length = std::min(block, count - offset);
            const double power = energy(samples + offset, length);
            if (power >= quietEnergy) {
                activeEnergy += power * length;
                activeSamples += length;
                consecutive += length;
                longest = std::max(longest, consecutive);
            } else consecutive = 0;
        }
        // Require 60 ms of sustained activity; a click must not turn a silent
        // window into a full Whisper inference. Tail silence doesn't dilute RMS.
        if (longest < 3 * block) return {};
        for (size_t i = 0; i < count; ++i) mean += samples[i];
        mean /= count;
        double peak = 0;
        for (size_t i = 0; i < count; ++i)
            peak = std::max(peak, std::abs(samples[i] - mean));
        const double rms = std::sqrt(activeEnergy / activeSamples);
        double gain = std::clamp(.05 / rms, 1.0, 8.0);
        // Boost by at most 18 dB, with headroom for peaks. Normal-volume audio
        // is not attenuated. Work on a copy so pending audio remains unchanged.
        if (peak > 0) gain = std::min(gain, std::max(1.0, .95 / peak));
        std::vector<float> result(count);
        for (size_t i = 0; i < count; ++i)
            result[i] = static_cast<float>((samples[i] - mean) * gain);
        return result;
    }
};
}
