#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace FluxPlayer::SettingsParsing {

inline constexpr int kWindowWidthMin = 320, kWindowWidthMax = 7680;
inline constexpr int kWindowHeightMin = 240, kWindowHeightMax = 4320;
inline constexpr int kTcpLogPortMin = 1, kTcpLogPortMax = 65535;
inline constexpr double kVolumeMin = 0.0, kVolumeMax = 1.0;
inline constexpr double kSubtitleFontScaleMin = 0.5, kSubtitleFontScaleMax = 4.0;

// All parsers leave out unchanged on failure. Only ranged settings clamp.
inline bool parseBool(const std::string& value, bool& out) {
    if (value == "true" || value == "1") { out = true; return true; }
    if (value == "false" || value == "0") { out = false; return true; }
    return false;
}

inline bool parseClampedInt(const std::string& value, int lo, int hi, int& out) {
    if (lo > hi) return false;
    try {
        std::size_t used = 0;
        const int parsed = std::stoi(value, &used);
        if (used != value.size()) return false;
        out = std::clamp(parsed, lo, hi);
        return true;
    } catch (const std::logic_error&) { // invalid_argument or out_of_range
        return false;
    }
}

inline bool parseFiniteDouble(const std::string& value, double& out) {
    try {
        std::size_t used = 0;
        const double parsed = std::stod(value, &used);
        if (used != value.size() || !std::isfinite(parsed)) return false;
        out = parsed;
        return true;
    } catch (const std::logic_error&) {
        return false;
    }
}

inline bool parseClampedDouble(const std::string& value, double lo, double hi, double& out) {
    double parsed;
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi || !parseFiniteDouble(value, parsed)) return false;
    out = std::clamp(parsed, lo, hi);
    return true;
}

inline bool parsePlaybackSpeed(const std::string& value, double& out) {
    double parsed;
    if (!parseFiniteDouble(value, parsed)) return false;
    constexpr double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0, 8.0, 16.0};
    for (double speed : speeds) {
        if (parsed == speed) { out = speed; return true; }
    }
    return false;
}

} // namespace FluxPlayer::SettingsParsing
