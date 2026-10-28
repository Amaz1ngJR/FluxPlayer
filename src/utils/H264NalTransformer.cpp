// SPDX-License-Identifier: GPL-3.0-only
// Integrates the GPL-3.0 CCTVVideoDownloader NAL implementation; see
// third_party/cctv_h5e/README.md for attribution and distribution requirements.
#include "FluxPlayer/utils/H264NalTransformer.h"
#include "FluxPlayer/utils/PathUtils.h"
#include "FluxPlayer/utils/Logger.h"
#include "cctv_h5e/cctv_h5e_decrypt.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <cctype>
#include <cstring>

namespace FluxPlayer {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}
std::string hostOf(const std::string& url) {
    if (!isHttpUrl(url)) return "";
    const auto start = url.find("://") + 3;
    return lower(url.substr(start, url.find_first_of(":/?#", start) - start));
}
bool domainIs(const std::string& host, const std::string& domain) {
    return host == domain || (host.size() > domain.size() &&
        host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
        host[host.size() - domain.size() - 1] == '.');
}
size_t startCode(const uint8_t* data, size_t size, size_t pos) {
    if (pos + 3 > size || data[pos] != 0 || data[pos + 1] != 0) return 0;
    if (data[pos + 2] == 1) return 3;
    if (pos + 4 <= size && data[pos + 2] == 0 && data[pos + 3] == 1) return 4;
    return 0;
}

size_t decryptLiveNal(uint8_t* nal, size_t size) {
    if (size < 40 || ((nal[0] & 31) != 1 && (nal[0] & 31) != 5)) return size;
    std::vector<size_t> positions;
    cctv_h5e::build_rbsp_map(nal, size, positions);
    if (positions.size() < 40) return size;
    uint8_t key[16];
    for (size_t i = 0; i < 16; ++i) key[i] = nal[positions[16 + i]];
    // No-type25 live streams encrypt the last complete 8-byte cell too. The
    // upstream legacy-VOD routine requires 80 bytes remaining, leaving this
    // tail encrypted and producing bottom-row macroblock errors on live CCTV.
    for (size_t pos = 32; pos + 8 <= positions.size(); pos += 80) {
        uint8_t block[8];
        for (size_t i = 0; i < 8; ++i) block[i] = nal[positions[pos + i]];
        cctv_h5e::tea_decrypt_block(block, block, key);
        for (size_t i = 0; i < 8; ++i) nal[positions[pos + i]] = block[i];
    }
    // Compact once, rather than memmoving the remaining NAL once per EPB.
    size_t output = 0;
    for (const auto source : positions) nal[output++] = nal[source];
    return output;
}
}

struct H264NalTransformer::Impl {
    CctvProtectionMode mode;
    cctv_h5e::Session session;
    size_t processedNals = 0;
    bool sawControlNal = false;
    explicit Impl(CctvProtectionMode value) : mode(value) {}
};

H264NalTransformer::H264NalTransformer(CctvProtectionMode mode) : impl_(std::make_unique<Impl>(mode)) { reset(); }
H264NalTransformer::~H264NalTransformer() = default;

CctvProtectionMode H264NalTransformer::detect(const std::string& mediaUrl,
                                            const std::string& pageUrl, bool observed) {
    if (!isHttpUrl(mediaUrl)) return CctvProtectionMode::None;
    const auto media = lower(mediaUrl.substr(0, mediaUrl.find_first_of("?#")));
    const auto page = lower(pageUrl.substr(0, pageUrl.find_first_of("?#")));
    const bool cctvPage = domainIs(hostOf(page), "cctv.com") || domainIs(hostOf(page), "cntv.cn");
    const auto host = hostOf(mediaUrl);
    const bool cctvCdn = domainIs(host, "cntv.cn") || domainIs(host, "cctv.com") ||
        domainIs(host, "liveplay.myqcloud.com") || domainIs(host, "cntv.myalicdn.com");
    if (!cctvPage && !cctvCdn) return CctvProtectionMode::None;
    if (media.find("/cdrm") != std::string::npos ||
        (observed && cctvPage && page.find("/live/") != std::string::npos))
        return CctvProtectionMode::ClassicLive;
    if (media.find("/asp/h5e/") != std::string::npos) return CctvProtectionMode::H5E;
    // enc2 and generic EME/DRM are intentionally unsupported, not guessed as H5E.
    return CctvProtectionMode::None;
}

void H264NalTransformer::reset() {
    impl_->session.reset();
    // CCTV live classic mode has no type25 enable NAL. VOD H5E must wait for
    // its enable record so plaintext fallback content is never decrypted twice.
    impl_->session.shouldDecrypt = impl_->mode == CctvProtectionMode::ClassicLive;
    impl_->sawControlNal = false;
}

bool H264NalTransformer::process(AVPacket* packet, std::string& error) {
    if (!packet || !packet->data || packet->size <= 0) return true;
    constexpr size_t maxPacketBytes = 16 * 1024 * 1024;
    const auto size = static_cast<size_t>(packet->size);
    if (size > maxPacketBytes) { error = "央视视频包过大，停止处理"; return false; }
    std::vector<std::pair<size_t, size_t>> starts;
    for (size_t pos = 0; pos + 3 <= size;) {
        const auto prefix = startCode(packet->data, size, pos);
        if (prefix) { starts.emplace_back(pos, prefix); pos += prefix; }
        else ++pos;
    }
    if (starts.empty()) { error = "央视保护流不是支持的 H.264 Annex B 格式"; return false; }
    // The demuxer can share/refcount packet buffers; never modify shared input.
    if (av_packet_make_writable(packet) < 0) { error = "无法分配央视视频处理缓冲"; return false; }
    size_t output = starts.front().first;
    for (size_t i = 0; i < starts.size(); ++i) {
        const auto pos = starts[i].first;
        const auto prefix = starts[i].second;
        const auto end = i + 1 < starts.size() ? starts[i + 1].first : size;
        const auto begin = pos + prefix;
        if (begin >= end) { error = "央视视频 NAL 数据不完整"; return false; }
        size_t length = end - begin;
        const auto nalType = packet->data[begin] & 31;
        if (nalType == 25) impl_->sawControlNal = true;
        if (impl_->mode == CctvProtectionMode::ClassicLive && !impl_->sawControlNal)
            length = decryptLiveNal(packet->data + begin, length);
        else impl_->session.on_nal(packet->data + begin, &length);
        if (length > end - begin) { error = "央视视频处理返回了无效长度"; return false; }
        std::memmove(packet->data + output, packet->data + pos, prefix);
        output += prefix;
        std::memmove(packet->data + output, packet->data + begin, length);
        output += length;
        ++impl_->processedNals;
    }
    packet->size = static_cast<int>(output);
    std::memset(packet->data + output, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    // Packet ownership, timestamps, flags and side data remain unchanged.
    if (impl_->processedNals == starts.size()) LOG_INFO("H264NalTransformer: 央视 NAL 处理已启用");
    return true;
}
} // namespace FluxPlayer
