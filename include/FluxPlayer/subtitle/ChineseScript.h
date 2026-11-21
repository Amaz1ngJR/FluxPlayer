#pragma once
#include <string>

namespace FluxPlayer {
// Whisper is trained mostly on Traditional Chinese for zh, so it emits forms
// like 請/後/輸 for Mandarin audio. Convert the standard Traditional-only forms
// to Simplified characters before display. This is a character mapping only: it
// never changes wording and never guesses at a homophone.
class ChineseScript {
public:
    static std::string simplify(const std::string& text);
    // True when the text contains a Traditional-only form that has a mapping.
    static bool containsTraditional(const std::string& text);
};
}
