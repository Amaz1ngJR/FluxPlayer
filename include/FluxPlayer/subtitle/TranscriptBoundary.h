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
            result.push_back({c, begin});
        }
        return result;
    }
    static bool equivalent(const std::string& left, const std::string& right) {
        const auto a = characters(left), b = characters(right);
        return !a.empty() && a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(), [](const Character& x, const Character& y) { return x.value == y.value; });
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
