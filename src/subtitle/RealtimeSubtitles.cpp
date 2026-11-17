#include "FluxPlayer/subtitle/RealtimeSubtitles.h"
#include "FluxPlayer/subtitle/SpeechChunker.h"
#include "FluxPlayer/subtitle/TranscriptBoundary.h"
#include "FluxPlayer/utils/Logger.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#ifdef FLUX_WHISPER
#include <whisper.h>
#endif
namespace FluxPlayer {
struct RealtimeSubtitles::Impl {
    struct Input {
        std::shared_ptr<AVFrame> frame;
        double pts;
        int serial;
        uint64_t epoch;
    };
    struct Caption { std::string text; double end; uint64_t epoch; double displayedAt = -1; };
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Input> queue;
    std::deque<Caption> captions;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> epoch{0};
    int serial = -1;
    bool eof = false;
    std::string state = "AI: loading model...";
    std::thread worker;
    std::string model, language, hints;
    bool gpu;
    Impl(std::string m, std::string l, bool g, std::string h)
        : model(std::move(m)), language(std::move(l)), hints(std::move(h)), gpu(g) {
        worker = std::thread([this] {
            try { run(); }
            catch (const std::exception& e) { setStatus(std::string("AI subtitles: ") + e.what()); }
            catch (...) { setStatus("AI subtitles: recognition failed"); }
        });
    }
    ~Impl() { stop(); if (worker.joinable()) worker.join(); }
    void stop() { stopping.store(true); ready.notify_all(); }
    void setStatus(std::string s) {
        LOG_INFO(s);
        std::lock_guard<std::mutex> lock(mutex);
        state = std::move(s);
    }
    void run() {
#ifndef FLUX_WHISPER
        setStatus("AI subtitles unavailable: build with FLUX_WHISPER_SOURCE_DIR");
#else
        if (model.empty()) { setStatus("AI: select a Whisper model in Settings, then reopen media"); return; }
        if (language != "auto" && whisper_lang_id(language.c_str()) < 0) {
            setStatus("AI: invalid recognition language"); return;
        }
        auto cp = whisper_context_default_params();
        cp.use_gpu = gpu;
        std::unique_ptr<whisper_context, decltype(&whisper_free)> ctx(
            whisper_init_from_file_with_params(model.c_str(), cp), whisper_free);
        if (!ctx) { setStatus("AI: failed to load model; check path and GPU setting"); return; }
        if (stopping.load()) return;
        setStatus("AI: listening...");
        SwrContext* swr = nullptr;
        struct SwrOwner { SwrContext*& value; ~SwrOwner() { swr_free(&value); } } owner{swr};
        AVChannelLayout layout{};
        struct LayoutOwner { AVChannelLayout& value; ~LayoutOwner() { av_channel_layout_uninit(&value); } } layoutOwner{layout};
        int rate = 0, format = -1;
        std::vector<float> samples;
        std::vector<float> overlap;
        bool overlapSpeech = false;
        std::string previousTranscript;
        SpeechChunker chunker;
        bool resetContext = true;
        double start = 0, expected = -1;
        uint64_t active = UINT64_MAX;
        struct Abort { Impl* self; uint64_t epoch; } abort{this, 0};
        auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        params.n_threads = 4;
        params.language = language.c_str();
        params.n_max_text_ctx = 192;
        params.print_progress = params.print_realtime = params.print_timestamps = false;
        params.beam_search.beam_size = 5;
        // The chunk already supplies media timing. Avoid decoding timestamps
        // and bound runaway/repetitive text on noisy or music-backed input.
        params.no_timestamps = true;
        params.single_segment = true;
        params.max_tokens = 160;
        params.temperature_inc = 0;
        params.suppress_nst = true;
        params.abort_callback = [](void* data) {
            auto* a = static_cast<Abort*>(data);
            return a->self->stopping.load() || a->epoch != a->self->epoch.load();
        };
        params.abort_callback_user_data = &abort;
        // Explicit history prevents a rejected retry or weak recognition from
        // becoming the next chunk's implicit prompt inside whisper.cpp.
        auto tokenize = [&](const std::string& text) {
            std::vector<whisper_token> tokens(2048);
            const int count = whisper_tokenize(ctx.get(), text.c_str(), tokens.data(), static_cast<int>(tokens.size()));
            if (count < 0) tokens.clear(); else tokens.resize(count);
            return tokens;
        };
        // Whisper's prompt is transcript context, not an instruction channel.
        // A style instruction can leak into the transcript on weak/noisy audio.
        auto hintTokens = tokenize(hints);
        if (hintTokens.size() > 96) hintTokens.resize(96);
        std::vector<whisper_token> history;
        struct Candidate {
            std::string text;
            double score = -100;
            double weakFraction = 1;
            bool repeated = false;
            bool truncated = false;
        };
        auto clearContinuity = [&] {
            resetContext = true;
            history.clear(); overlap.clear(); previousTranscript.clear();
            overlapSpeech = false;
        };
        auto recognize = [&](size_t n, size_t retain = 0, bool forced = false) {
            if (n < 6400 || active != epoch.load() || stopping.load()) return;
            // Overlap must never turn an otherwise silent new chunk into speech.
            auto input = SpeechAudio::prepare(samples.data(), n);
            LOG_DEBUG("AI chunk: start=" + std::to_string(start) + " duration=" + std::to_string(n / 16000.) + " accepted=" + std::to_string(!input.empty()));
            if (input.empty()) { clearContinuity(); return; }
            // Keep the chunker's timeline disjoint. Only the inference input
            // includes the previous 480 ms, so EOF never replays an overlap alone.
            if (!overlap.empty()) {
                std::vector<float> audio = overlap;
                audio.insert(audio.end(), samples.begin(), samples.begin() + n);
                input = SpeechAudio::prepare(audio.data(), audio.size());
                if (input.empty()) { clearContinuity(); return; }
            }
            abort.epoch = active;
            auto decode = [&](bool useHistory, bool useHints, int beams) {
                auto tokens = useHints ? hintTokens : std::vector<whisper_token>{};
                if (useHistory && !resetContext) tokens.insert(tokens.end(), history.begin(), history.end());
                params.no_context = true;
                params.prompt_tokens = tokens.empty() ? nullptr : tokens.data();
                params.prompt_n_tokens = static_cast<int>(tokens.size());
                params.beam_search.beam_size = beams;
                Candidate result;
                if (whisper_full(ctx.get(), params, input.data(), static_cast<int>(input.size())) != 0) {
                    if (active == epoch.load() && !stopping.load()) throw std::runtime_error("inference failed");
                    return result;
                }
                double total = 0; size_t count = 0, weak = 0;
                std::vector<whisper_token> outputTokens;
                for (int i = 0; i < whisper_full_n_segments(ctx.get()); ++i) {
                    double segmentTotal = 0;
                    std::vector<whisper_token> segmentTokens;
                    for (int j = 0; j < whisper_full_n_tokens(ctx.get(), i); ++j) {
                        const auto token = whisper_full_get_token_data(ctx.get(), i, j);
                        if (token.id >= whisper_token_eot(ctx.get())) continue;
                        segmentTotal += std::log(std::max(1e-6f, token.p));
                        segmentTokens.push_back(token.id);
                    }
                    if (segmentTokens.empty()) continue;
                    // Combine acoustic and decoding evidence: quiet volume by
                    // itself is not a reason to throw away genuine speech.
                    if (whisper_full_get_segment_no_speech_prob(ctx.get(), i) > .8f &&
                        segmentTotal / segmentTokens.size() < -1.2) continue;
                    result.text += whisper_full_get_segment_text(ctx.get(), i);
                    total += segmentTotal; count += segmentTokens.size();
                    for (int j = 0; j < whisper_full_n_tokens(ctx.get(), i); ++j) {
                        const auto token = whisper_full_get_token_data(ctx.get(), i, j);
                        if (token.id < whisper_token_eot(ctx.get()) && token.p < .2f) ++weak;
                    }
                    outputTokens.insert(outputTokens.end(), segmentTokens.begin(), segmentTokens.end());
                }
                if (count) result.score = total / count;
                if (count) result.weakFraction = static_cast<double>(weak) / count;
                result.truncated = count >= static_cast<size_t>(params.max_tokens);
                // Three consecutive copies of a multi-token phrase are a
                // decoding warning, not proof of what the speaker said.
                for (size_t length = 2; length <= 8; ++length) {
                    for (size_t offset = 0; offset + 3 * length <= outputTokens.size(); ++offset) {
                        const auto first = outputTokens.begin() + offset;
                        if (std::equal(first, first + length, first + length) &&
                            std::equal(first, first + length, first + 2 * length)) result.repeated = true;
                    }
                }
                return result;
            };
            // Acoustic-first: an inherited mistake cannot bias the first pass.
            auto result = decode(false, true, 5);
            if (active != epoch.load() || stopping.load()) return;
            bool behind = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                behind = queue.size() > 1 && queue.back().pts - queue.front().pts >= .75;
            }
            bool corroborated = false;
            const bool reliable = result.score >= -.35 && result.weakFraction <= .1 &&
                !result.repeated && !result.truncated;
            if (!behind && (result.score < -.8 || result.weakFraction > .2 ||
                            result.repeated || result.truncated || reliable || !resetContext)) {
                // One verification at most. A reliable previous transcript may
                // influence only this second pass, never the acoustic baseline.
                const auto retry = decode(!resetContext && reliable, !result.repeated, 8);
                corroborated = reliable && retry.score >= -.35 && retry.weakFraction <= .1 &&
                    !retry.repeated && !retry.truncated &&
                    TranscriptBoundary::equivalent(result.text, retry.text);
                const bool comparable = result.text.empty() ||
                    (retry.text.size() * 2 >= result.text.size() && retry.text.size() <= result.text.size() * 2);
                // Disagreement never seeds history. Require a meaningful score
                // margin and no increase in weak tokens before replacing text.
                if (!retry.text.empty() && !retry.repeated && !retry.truncated &&
                    (result.repeated || result.text.empty() ||
                     (comparable && retry.score > result.score + .15 &&
                      retry.weakFraction <= result.weakFraction))) result = retry;
            }
            if (active != epoch.load() || stopping.load()) return;
            if (result.text.find_first_not_of(" \r\n\t") == std::string::npos) { clearContinuity(); return; }
            history.clear();
            // Agreement is additional evidence, not proof of correctness. Keep
            // only a small, independently corroborated transcript as history.
            resetContext = !corroborated;
            if (!resetContext) {
                history = tokenize(result.text);
                if (history.size() > 24) history.erase(history.begin(), history.end() - 24);
            }
            auto displayText = !overlapSpeech ? result.text :
                TranscriptBoundary::removeOverlap(previousTranscript, result.text);
            previousTranscript = result.text;
            // Raw PCM avoids applying automatic gain twice to the overlap.
            // At a natural pause retain only the pause, not half of its last
            // spoken word. Forced cuts need spoken lookback and text dedup.
            const size_t retained = std::min(n, retain);
            overlap.assign(samples.begin() + n - retained, samples.begin() + n);
            overlapSpeech = forced && retained > 0;
            if (displayText.empty()) return;
            std::lock_guard<std::mutex> lock(mutex);
            if (active != epoch.load() || stopping.load()) return;
            captions.push_back({std::move(displayText), start + n / 16000., active});
            LOG_DEBUG("AI result: end=" + std::to_string(captions.back().end) + " bytes=" + std::to_string(captions.back().text.size()) + " score=" + std::to_string(result.score) + " history_verified=" + std::to_string(corroborated));
            while (captions.size() > 8) captions.pop_front();
        };
        while (!stopping.load()) {
            Input in{};
            bool tail = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopping.load() || !queue.empty() || eof || active != epoch.load(); });
                if (stopping.load()) break;
                if (active != epoch.load()) {
                    active = epoch.load(); samples.clear(); expected = -1; swr_free(&swr);
                    chunker.reset(); clearContinuity();
                }
                if (!queue.empty()) { in = std::move(queue.front()); queue.pop_front(); }
                else if (eof) { eof = false; tail = true; }
                else continue;
            }
            if (tail) {
                recognize(samples.size()); samples.clear(); expected = -1;
                chunker.reset(); clearContinuity();
                continue;
            }
            if (in.epoch != active) continue;
            const auto* f = in.frame.get();
            if (f->sample_rate <= 0 || f->ch_layout.nb_channels <= 0) continue;
            const bool gap = expected >= 0 && std::abs(in.pts - expected) > .25;
            const bool changed = rate != f->sample_rate || format != f->format ||
                av_channel_layout_compare(&layout, &f->ch_layout) != 0;
            if (gap || changed || !swr) {
                samples.clear(); start = in.pts;
                chunker.reset(); clearContinuity();
                swr_free(&swr);
                av_channel_layout_uninit(&layout);
                if (av_channel_layout_copy(&layout, &f->ch_layout) < 0) throw std::runtime_error("audio layout allocation failed");
                AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
                if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, 16000,
                    &layout, static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr) < 0 ||
                    swr_init(swr) < 0) throw std::runtime_error("audio resampler failed");
                rate = f->sample_rate; format = f->format;
            }
            if (samples.empty()) start = in.pts;
            expected = in.pts + static_cast<double>(f->nb_samples) / rate;
            const int capacity = static_cast<int>(av_rescale_rnd(swr_get_delay(swr, rate) + f->nb_samples,
                16000, rate, AV_ROUND_UP));
            std::vector<float> converted(capacity);
            auto* out = reinterpret_cast<uint8_t*>(converted.data());
            const int n = swr_convert(swr, &out, capacity,
                const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
            if (n < 0) throw std::runtime_error("audio conversion failed");
            samples.insert(samples.end(), converted.begin(), converted.begin() + n);
            while (active == epoch.load() && !stopping.load()) {
                const auto boundary = chunker.next(samples);
                if (!boundary.count) break;
                if (boundary.recognize) recognize(boundary.count, boundary.overlap, boundary.forced);
                else clearContinuity();
                samples.erase(samples.begin(), samples.begin() + boundary.count);
                start += boundary.count / 16000.;
                chunker.reset();
            }
        }
#endif
    }
};
RealtimeSubtitles::RealtimeSubtitles(std::string m, std::string l, bool g, std::string h)
    : impl_(std::make_unique<Impl>(std::move(m), std::move(l), g, std::move(h))) {}
RealtimeSubtitles::~RealtimeSubtitles() = default;
void RealtimeSubtitles::requestStop() { impl_->stop(); }
void RealtimeSubtitles::reset(int serial) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->serial == serial) return;
    impl_->serial = serial;
    ++impl_->epoch;
    impl_->queue.clear(); impl_->captions.clear(); impl_->eof = false;
    impl_->ready.notify_one();
}
void RealtimeSubtitles::push(const AVFrame* frame, double pts, int serial) {
    if (!frame || !std::isfinite(pts) || impl_->stopping.load()) return;
    auto copy = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* p) { av_frame_free(&p); });
    if (!copy) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->serial != serial || impl_->stopping.load()) return;
    // Bounded memory even while loading a model or playing at high speed.
    while (impl_->queue.size() >= 64) impl_->queue.pop_front();
    impl_->queue.push_back({std::move(copy), pts, serial, impl_->epoch.load()});
    impl_->ready.notify_one();
}
void RealtimeSubtitles::finish(int serial) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (serial == impl_->serial) { impl_->eof = true; impl_->ready.notify_one(); }
}
std::string RealtimeSubtitles::text(double now, int serial) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (serial != impl_->serial || !std::isfinite(now)) return {};
    // Use media time: pause freezes visibility, speed changes do not alter PTS.
    // Decoding can run ahead; never display captions before their source audio ends.
    Impl::Caption* latest = nullptr;
    for (auto& caption : impl_->captions)
        if (caption.epoch == impl_->epoch.load() && now >= caption.end - .1)
            latest = &caption;
    if (!latest) return {};
    // Inference can finish seconds after the source chunk. Give the newest
    // eligible caption its reading time from first display, not from audio end.
    // Media time still freezes expiry while paused; a new result replaces it.
    if (latest->displayedAt < 0) {
        latest->displayedAt = now;
        LOG_DEBUG("AI display: media=" + std::to_string(now) + " end=" + std::to_string(latest->end));
    }
    return now < latest->displayedAt + 6 ? latest->text : std::string{};
}
std::string RealtimeSubtitles::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}
}
