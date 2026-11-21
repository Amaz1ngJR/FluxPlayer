#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace FluxPlayer {
// Match script-independent text while retaining UTF-8 byte boundaries. Ignore
// spacing, case (ASCII) and common punctuation, never kana or CJK characters.
struct TranscriptBoundary {
    struct Character { uint32_t value; size_t byte; };
    static bool latinWord(uint32_t c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }
    static std::vector<Character> characters(const std::string& text) {
        std::vector<Character> result;
        size_t last = 0;
        for (size_t i = 0; i < text.size();) {
            const size_t begin = i;
            uint32_t c = static_cast<unsigned char>(text[i++]);
            int remaining = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
            if (remaining) c &= (1u << (6 - remaining)) - 1;
            while (remaining-- > 0 && i < text.size()) c = (c << 6) | (static_cast<unsigned char>(text[i++]) & 0x3f);
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c <= 0x20 || (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) ||
                (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e) ||
                c == 0x3000 || c == 0x3001 || c == 0x3002 ||
                (c >= 0x2010 && c <= 0x201f) || c == 0xff01 || c == 0xff0c ||
                c == 0xff0e || c == 0xff1a || c == 0xff1b || c == 0xff1f) continue;
            // A dropped character leaves a gap: two visible characters can share
            // a byte offset only if they are the same character. Callers locate
            // text by this offset, so keep it strictly increasing.
            const size_t byte = begin <= last ? last : begin;
            result.push_back({c, byte});
            last = byte + 1;
        }
        return result;
    }
    static bool equivalent(const std::string& left, const std::string& right) {
        const auto a = characters(left), b = characters(right);
        return !a.empty() && a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(), [](const Character& x, const Character& y) { return x.value == y.value; });
    }
    static bool matchesPrefix(const std::string& prefix, const std::string& text) {
        const auto a = characters(prefix), b = characters(text);
        return !a.empty() && a.size() <= b.size() &&
            std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
                return x.value == y.value;
            });
    }
    static bool hasAnchoredPreviewClue(const std::string& preview, const std::string& final) {
        const auto a = characters(preview), b = characters(final);
        if (a.size() < 8 || a.size() + 2 > b.size()) return false;
        size_t differences = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].value == b[i].value) continue;
            if (i < 3 || i + 1 >= a.size() || ++differences > 1) return false;
        }
        return differences == 1;
    }
    // A delayed revision should add a phrase also heard in the new audio.
    static bool addsSharedPair(const std::string& original, const std::string& revised,
                               const std::string& clue) {
        const auto before = characters(original);
        const auto after = characters(revised);
        const auto later = characters(clue);
        const auto isHan = [](uint32_t c) {
            return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff);
        };
        auto containsPair = [](const auto& chars, uint32_t first, uint32_t second) {
            for (size_t i = 1; i < chars.size(); ++i)
                if (chars[i - 1].value == first && chars[i].value == second) return true;
            return false;
        };
        for (size_t i = 1; i < after.size(); ++i) {
            const auto first = after[i - 1].value;
            const auto second = after[i].value;
            if (isHan(first) && isHan(second) && containsPair(later, first, second) &&
                !containsPair(before, first, second)) return true;
        }
        return false;
    }
    // Use the preview as evidence only where the two full-window candidates
    // have a small, locally aligned disagreement.
    static bool previewSupports(const std::string& preview, const std::string& baseline,
                                const std::string& alternative) {
        const auto draft = characters(preview);
        const auto first = characters(baseline);
        const auto second = characters(alternative);
        if (first.size() != second.size() || draft.size() < 6) return false;
        size_t differences = 0;
        for (size_t i = 0; i < first.size(); ++i) {
            if (first[i].value == second[i].value) continue;
            if (i >= draft.size() || draft[i].value != second[i].value) return false;
            const auto anchored = [&](size_t index) {
                return index < draft.size() && draft[index].value == first[index].value &&
                    first[index].value == second[index].value;
            };
            if (!(i > 0 && anchored(i - 1)) && !(i + 1 < first.size() && anchored(i + 1)))
                return false;
            if (++differences > 1) return false;
        }
        return differences == 1;
    }
    // Only call when the input audio actually overlaps the previous chunk.
    // Conservative exact suffix/prefix matching avoids fuzzy deletion of speech.
    static std::string removeOverlap(const std::string& previous, const std::string& current) {
        const auto a = characters(previous), b = characters(current);
        for (size_t count = std::min({a.size(), b.size(), size_t(16)}); count >= 2; --count) {
            // A whole new utterance may legitimately repeat the previous one.
            // Keep that ambiguous case rather than erase all its speech.
            if (count == b.size()) continue;
            size_t budget = 0;
            for (size_t i = 0; i < count; ++i) budget += b[i].value < 0x80 ? 1 : 3;
            if (budget > 16) continue; // 480 ms cannot contain a long sentence.
            const size_t begin = a.size() - count;
            if (!std::equal(a.begin() + begin, a.end(), b.begin(),
                    [](const Character& x, const Character& y) { return x.value == y.value; })) continue;
            const bool latin = std::all_of(b.begin(), b.begin() + count,
                [](const Character& c) { return latinWord(c.value); });
            if (latin && count < 3) continue;
            // A Latin match must contain whole words, including at each edge.
            if (begin && latinWord(a[begin].value) && latinWord(a[begin - 1].value) &&
                a[begin].byte == a[begin - 1].byte + 1) continue;
            if (count < b.size() && latinWord(b[count - 1].value) && latinWord(b[count].value) &&
                b[count].byte == b[count - 1].byte + 1) continue;
            return current.substr(b[count].byte);
        }
        return current;
    }
};
}
