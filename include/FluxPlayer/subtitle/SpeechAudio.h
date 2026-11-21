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
    // Tail trimming uses a higher gate than the low-volume detector: residual
    // noise below this level adds nothing for whisper and invites stray text.
    static constexpr double speechEnergy = quietEnergy * 20; // about -57 dBFS
    static constexpr size_t maxTailSilence = 3200; // 200 ms

    // Smooth gain reduction for loud passages ahead of whisper's fixed mel
    // normalization. Peaks are compressed rather than clipped, so quiet
    // syllables in the same window keep their relative level.
    static void compress(std::vector<float>& samples) {
        constexpr double threshold = .25; // about -12 dBFS
        constexpr double ratio = 3.0;
        constexpr double attack = .01;    // per sample at 16 kHz
        constexpr double release = .0005;
        double envelope = 0;
        for (auto& sample : samples) {
            const double level = std::abs(sample);
            envelope = level > envelope ? envelope + (level - envelope) * attack
                                        : envelope + (level - envelope) * release;
            if (envelope <= threshold) continue;
            const double target = threshold + (envelope - threshold) / ratio;
            sample = static_cast<float>(sample * target / envelope);
        }
    }

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
        // Drop trailing near-silence but keep 200 ms of context for the last
        // syllable, so the window never ends in undecodable noise.
        size_t speechEnd = 0;
        for (size_t offset = 0; offset + block <= count; offset += block)
            if (energy(result.data() + offset, block) >= speechEnergy) speechEnd = offset + block;
        if (speechEnd && speechEnd + maxTailSilence < count)
            result.resize(speechEnd + maxTailSilence);
        return result;
    }
};
}
