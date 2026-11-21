#pragma once
#include "FluxPlayer/subtitle/SpeechAudio.h"
#include <cstddef>
#include <vector>

namespace FluxPlayer {
// Energy-based boundaries for 16 kHz mono PCM. This is not a speech VAD.
// The caller consumes count samples, advances their media PTS, then resets us.
class SpeechChunker {
public:
    struct Boundary {
        size_t count = 0;
        bool recognize = false;
        size_t overlap = 0;
        bool forced = false;
    };
    static constexpr size_t sampleRate = 16000;
    static constexpr size_t block = SpeechAudio::block; // 20 ms
    static constexpr double quietEnergy = SpeechAudio::quietEnergy;

    void reset() { analyzed_ = quiet_ = 0; hasSound_ = false; }

    Boundary next(const std::vector<float>& samples) {
        while (analyzed_ + block <= samples.size()) {
            const bool quiet = SpeechAudio::energy(samples.data() + analyzed_, block) < quietEnergy;
            quiet_ = quiet ? quiet_ + block : 0;
            hasSound_ = hasSound_ || !quiet;
            analyzed_ += block;

            // Keep 200 ms before sound starts; long silence does not become a
            // large recognition window or count toward the next sentence.
            if (!hasSound_ && analyzed_ >= sampleRate)
                return {analyzed_ - sampleRate / 5, false};

            // Prefer a real pause to a cut through a word. Longer utterances
            // can close on a 160 ms breath; very short gaps keep their context.
            if (hasSound_ && analyzed_ >= sampleRate * 6 / 5 &&
                quiet_ >= (analyzed_ >= sampleRate * 3 ? 2560u : 5120u))
                return {analyzed_, true, std::min(quiet_, size_t(7680)), false};

            // In a long phrase, accept a quiet pause with residual room noise.
            // Require both low absolute energy and a sharp drop from the
            // preceding audio, so ordinary soft syllables don't end the chunk.
            if (hasSound_ && analyzed_ >= sampleRate * 9 / 2) {
                constexpr size_t pause = 6 * block;
                constexpr size_t preceding = 14 * block;
                const auto pauseEnergy = SpeechAudio::energy(samples.data() + analyzed_ - pause, pause);
                const auto precedingEnergy = SpeechAudio::energy(
                    samples.data() + analyzed_ - pause - preceding, preceding);
                if (pauseEnergy < 1e-5 && pauseEnergy < precedingEnergy * .005)
                    return {analyzed_, true, pause, false};
            }

            // Hard limit. Shorter than the previous 8 s because the bundled
            // model re-decodes a whole window per refresh: a smaller ceiling
            // bounds both the visible latency and the per-refresh cost.
            if (analyzed_ >= sampleRate * 6) {
                // At the hard limit, prefer a quiet 80 ms region in the last
                // 800 ms. Only use it if clearly quieter than that whole tail;
                // otherwise avoid arbitrarily shortening continuous speech.
                const size_t begin = analyzed_ - sampleRate * 4 / 5;
                double lowest = 1e100;
                size_t cut = analyzed_;
                for (size_t offset = begin; offset + 4 * block <= analyzed_; offset += block) {
                    const double sum = SpeechAudio::energy(samples.data() + offset, 4 * block);
                    if (sum < lowest) { lowest = sum; cut = offset + 2 * block; }
                }
                const double tailEnergy = SpeechAudio::energy(samples.data() + begin, analyzed_ - begin);
                return {lowest < tailEnergy * .35 ? cut : analyzed_, true, 7680, true};
            }
        }
        return {};
    }

private:
    size_t analyzed_ = 0, quiet_ = 0;
    bool hasSound_ = false;
};
}
