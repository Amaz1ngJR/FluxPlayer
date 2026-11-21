#pragma once
#include <string>
#include <algorithm>
#include "FluxPlayer/subtitle/TranscriptBoundary.h"
namespace FluxPlayer {
// Two consecutive growing-audio hypotheses stabilize a UTF-8 preview prefix.
// The final full-window result is published separately.
struct LocalAgreement {
    std::string previous, stable, pending;
    void reset() { previous.clear(); stable.clear(); pending.clear(); }
    void update(std::string text, bool trustworthy = true) {
        // Punctuation, spacing and ASCII case may change across hypotheses.
        // Align the committed content before comparing the still-editable tail.
        const auto committed = TranscriptBoundary::characters(stable);
        const auto incoming = TranscriptBoundary::characters(text);
        if (!committed.empty()) {
            if (incoming.size() < committed.size() || !std::equal(committed.begin(), committed.end(), incoming.begin(),
                    [](const TranscriptBoundary::Character& a, const TranscriptBoundary::Character& b) { return a.value == b.value; })) {
                previous.clear();
                return;
            }
            auto endOf = [](const std::string& value, size_t byte) {
                ++byte;
                while (byte < value.size() && (static_cast<unsigned char>(value[byte]) & 0xc0) == 0x80) ++byte;
                return byte;
            };
            stable.resize(endOf(stable, committed.back().byte));
            text = stable + text.substr(endOf(text, incoming[committed.size()-1].byte));
        }
        if (!trustworthy) { pending = text.substr(stable.size()); previous.clear(); return; }
        size_t n = 0;
        while (n < previous.size() && n < text.size() && previous[n] == text[n]) ++n;
        while (n < text.size() && n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
        // Do not confirm half of an English word (e.g. account/accounting).
        auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
        if (n && n < text.size() && letter(text[n]) && letter(text[n-1]))
            while (n && letter(text[n-1])) --n;
        if (n > stable.size()) stable = text.substr(0, n);
        pending = text.substr(stable.size());
        previous = text;
    }
};
}
