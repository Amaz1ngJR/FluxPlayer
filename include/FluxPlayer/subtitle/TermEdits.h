#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace FluxPlayer {
// Explicit, user-authored terminology corrections. Unlike fuzzy homophone
// replacement, only the exact wrong form is rewritten, and only when the audio
// window actually contains a word that sounds like the intended term.
class TermEdits {
public:
    // Sound of a term: one hashed syllable per Chinese character plus the
    // number of BPE tokens the bundled model spends on it. Lower cost wins.
    struct Sound {
        std::array<uint32_t, 12> slots{};
        size_t length = 0;
        int cost = 0;
        bool operator==(const Sound& other) const;
    };
    struct Rule {
        std::string wrong;
        std::string right;
        Sound pinyin;
    };
    // "wrong=right" separated by commas or newlines. Duplicates, empty sides,
    // oversized terms and a term mapped to itself are ignored.
    bool parse(const std::string& spec);
    bool empty() const { return rules_.empty(); }
    // Replace every occurrence that has nearby audio support, then drop chained
    // rules ("A=B, B=C") so one result is never corrected twice.
    std::string apply(const std::string& text, const std::string& heard) const;
    static Sound pinyinKey(const std::string& text);

private:
    std::vector<Rule> rules_;
};
}
