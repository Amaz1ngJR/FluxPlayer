#include "FluxPlayer/subtitle/RealtimeSubtitles.h"
#include "FluxPlayer/subtitle/SpeechChunker.h"
#include "FluxPlayer/subtitle/TranscriptBoundary.h"
#include "FluxPlayer/subtitle/LocalAgreement.h"
#include "FluxPlayer/subtitle/TermEdits.h"
#include "FluxPlayer/subtitle/ChineseScript.h"
#include "FluxPlayer/utils/Logger.h"
#include <atomic>
#include <chrono>
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
    struct Caption { std::string text; double end; uint64_t epoch; double displayedAt = -1; std::string pending; };
    struct Correction {
        std::string text;
        double availableAt = 0;
        double displayedAt = -1;
    };
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Input> queue;
    std::deque<Caption> captions;
    Correction correction;
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
        LocalAgreement agreement;
        std::deque<std::string> previewClues;
        size_t lastRecognition = 0;
        double start = 0, expected = -1;
        uint64_t active = UINT64_MAX;
        using Clock = std::chrono::steady_clock;
        struct Abort {
            Impl* self;
            uint64_t epoch;
            Clock::time_point deadline = Clock::time_point::max();
        } abort{this, 0};
        auto params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        params.n_threads = 4;
        params.language = language.c_str();
        params.n_max_text_ctx = 192;
        params.print_progress = params.print_realtime = params.print_timestamps = false;
        params.beam_search.beam_size = 5;
        // Media timing still comes from the audio chunks themselves.
        params.single_segment = true;
        params.max_tokens = 160;
        params.temperature_inc = 0;
        params.suppress_nst = true;
        params.detect_language = false;
        params.abort_callback = [](void* data) {
            auto* a = static_cast<Abort*>(data);
            return a->self->stopping.load() || a->epoch != a->self->epoch.load() ||
                Clock::now() >= a->deadline;
        };
        params.abort_callback_user_data = &abort;
        // Retranscriptions use only explicit vocabulary hints. Draft text never
        // becomes an implicit prompt inside whisper.cpp.
        auto tokenize = [&](const std::string& text) {
            std::vector<whisper_token> tokens(2048);
            const int count = whisper_tokenize(ctx.get(), text.c_str(), tokens.data(), static_cast<int>(tokens.size()));
            if (count < 0) tokens.clear(); else tokens.resize(count);
            return tokens;
        };
        // Whisper's prompt is transcript context, not an instruction channel.
        // A style instruction can leak into the transcript on weak/noisy audio.
        auto hintTokens = tokenize(hints);
        if (hintTokens.size() > 32) hintTokens.resize(32);
        // Explicit user terminology. Only exact wrong forms are rewritten, and
        // only when the audio window supports the intended reading.
        TermEdits termEdits;
        termEdits.parse(hints);
        std::vector<whisper_token> precedingText;
        struct Candidate {
            std::string text;
            double score = -100;
            double weakFraction = 1;
            bool repeated = false;
            bool truncated = false;
            bool usable() const {
                return !repeated && !truncated && text.find_first_not_of(" \r\n\t") != std::string::npos;
            }
        };
        struct ReviewWindow {
            std::vector<float> audio;
            std::string overlapText;
            double end = 0;
            double score = -100;
            double weakFraction = 1;
        };
        std::deque<ReviewWindow> reviewWindows;
        auto clearContinuity = [&] {
            overlap.clear(); previousTranscript.clear();
            precedingText.clear();
            reviewWindows.clear();
            overlapSpeech = false;
            agreement.reset(); lastRecognition = 0;
            previewClues.clear();
        };
        auto recognize = [&](size_t n, size_t retain = 0, bool forced = false, bool closing = true) {
            if (n < 6400 || active != epoch.load() || stopping.load()) return;
            // Overlap must never turn an otherwise silent new chunk into speech.
            auto input = SpeechAudio::prepare(samples.data(), n);
            LOG_DEBUG("AI chunk: start=" + std::to_string(start) + " duration=" + std::to_string(n / 16000.) + " accepted=" + std::to_string(!input.empty()));
            if (input.empty()) { clearContinuity(); return; }
            SpeechAudio::compress(input);
            // Keep the chunker's timeline disjoint. Only the inference input
            // includes the previous 480 ms, so EOF never replays an overlap alone.
            if (!overlap.empty()) {
                std::vector<float> audio = overlap;
                audio.insert(audio.end(), samples.begin(), samples.begin() + n);
                input = SpeechAudio::prepare(audio.data(), audio.size());
                if (input.empty()) { clearContinuity(); return; }
                SpeechAudio::compress(input);
            }
            abort.epoch = active;
            bool retryUsed = false;
            auto decode = [&](const std::vector<float>& audio, bool withTimestamps,
                              const std::vector<whisper_token>& tokens, bool optional = false) {
                if (optional) retryUsed = true;
                abort.deadline = optional ? Clock::now() + std::chrono::milliseconds(650) :
                    Clock::time_point::max();
                params.no_context = true;
                // The encoder runs over its whole context, so a 6 s chunk would
                // otherwise pay for the full 30 s (1500). A context of N covers
                // 2*N mel frames, and the mel has one frame per 10 ms, so N =
                // audio_ms / 20, plus a wide margin: the encoder attends across
                // its context, and cutting it too tight drops trailing words.
                const int audioMs = static_cast<int>(audio.size() / 16);
                params.audio_ctx = std::clamp((audioMs + 1600) / 20, 750, 1500);
                params.prompt_tokens = tokens.empty() ? nullptr : tokens.data();
                params.prompt_n_tokens = static_cast<int>(tokens.size());
                params.no_timestamps = !withTimestamps;
                Candidate result;
                const int decoded = whisper_full(ctx.get(), params, audio.data(), static_cast<int>(audio.size()));
                if (Clock::now() >= abort.deadline) {
                    LOG_DEBUG("AI optional subtitle review timed out");
                    return result;
                }
                if (decoded != 0) {
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
            // Chinese final decoding can use the model's timing tokens to
            // improve word selection. Keep the fast preview path and preserve
            // the text-only fallback if timing yields no usable hypothesis.
            const bool withTimestamps = closing && language == "zh";
            auto result = decode(input, withTimestamps, hintTokens);
            if (withTimestamps && !result.usable() && active == epoch.load() && !stopping.load())
                result = decode(input, false, hintTokens, true);
            if (active != epoch.load() || stopping.load()) return;
            const double end = start + n / 16000.;
            auto hasRetryRoom = [&] {
                if (retryUsed) return false;
                std::lock_guard<std::mutex> lock(mutex);
                return queue.size() < 2 || queue.back().pts - queue.front().pts < .45;
            };
            const bool reviewIsRecent = !reviewWindows.empty() &&
                start - reviewWindows.back().end < .3 && end - reviewWindows.front().end <= 12;
            std::vector<whisper_token> followingClue;
            if (withTimestamps) {
                const auto independent = result;
                const auto preview = agreement.stable + agreement.pending;
                const auto draft = TranscriptBoundary::characters(preview);
                const auto final = TranscriptBoundary::characters(result.text);
                const bool previewDisagrees = draft.size() >= 6 && draft.size() <= final.size() &&
                    !std::equal(draft.begin(), draft.end(), final.begin(),
                        [](const auto& a, const auto& b) { return a.value == b.value; });
                // A strong downstream clue can reach a window on its last
                // eligible boundary. Reserve the retry for that older window.
                const bool olderReviewHasClue = reviewIsRecent && reviewWindows.size() == 2 &&
                    result.score >= -.35 && result.weakFraction <= .1 && hintTokens.size() < 96;
                auto clue = std::find_if(previewClues.rbegin(), previewClues.rend(), [&](const auto& text) {
                    return TranscriptBoundary::hasAnchoredPreviewClue(text, result.text);
                });
                if (result.usable() && !overlapSpeech && clue != previewClues.rend() && hasRetryRoom()) {
                    auto clueTokens = tokenize(*clue);
                    if (!clueTokens.empty() && clueTokens.size() <= 32 &&
                        hintTokens.size() + clueTokens.size() <= 96) {
                        auto prompt = hintTokens;
                        prompt.insert(prompt.end(), clueTokens.begin(), clueTokens.end());
                        auto alternative = decode(input, true, prompt, true);
                        if (active != epoch.load() || stopping.load()) return;
                        const auto revised = TranscriptBoundary::characters(alternative.text).size();
                        if (alternative.usable() && alternative.score >= result.score + .06 &&
                            alternative.weakFraction <= result.weakFraction &&
                            revised * 5 >= final.size() * 4 && revised * 4 <= final.size() * 5 &&
                            TranscriptBoundary::matchesPrefix(*clue, alternative.text))
                            result = std::move(alternative);
                    }
                }
                if (result.usable() && previewDisagrees && !olderReviewHasClue && !overlapSpeech && hasRetryRoom()) {
                    auto alternative = decode(input, false, hintTokens, true);
                    if (active != epoch.load() || stopping.load()) return;
                    if (alternative.usable() && alternative.score >= result.score &&
                        alternative.weakFraction <= result.weakFraction &&
                        TranscriptBoundary::previewSupports(preview, result.text, alternative.text)) {
                        result = std::move(alternative);
                    }
                }
                // Only the independent pass can seed future context. Keep the
                // contextual pass optional when audio is already backing up.
                auto context = std::move(precedingText);
                precedingText.clear();
                if (independent.usable() && independent.score >= -.55 && independent.weakFraction <= .2) {
                    precedingText = tokenize(independent.text);
                    const auto limit = std::min<size_t>(32, 96 - hintTokens.size());
                    if (independent.score >= -.35 && independent.weakFraction <= .1) {
                        followingClue = precedingText;
                        if (followingClue.size() > limit) followingClue.resize(limit);
                    }
                    if (precedingText.size() > limit)
                        precedingText.erase(precedingText.begin(), precedingText.end() - limit);
                }
                if (hasRetryRoom() && !context.empty() && result.usable() &&
                    (!reviewIsRecent || followingClue.empty())) {
                    auto prompt = hintTokens;
                    prompt.insert(prompt.end(), context.begin(), context.end());
                    auto alternative = decode(input, true, prompt, true);
                    if (active != epoch.load() || stopping.load()) return;
                    if (alternative.usable() && alternative.score >= result.score + .10 &&
                        alternative.weakFraction <= result.weakFraction) {
                        const auto original = TranscriptBoundary::characters(result.text).size();
                        const auto revised = TranscriptBoundary::characters(alternative.text).size();
                        if (original && revised * 5 >= original * 4 && revised * 4 <= original * 5)
                            result = std::move(alternative);
                    }
                }
            }
            lastRecognition = n;
            auto displayText = !overlapSpeech ? result.text :
                TranscriptBoundary::removeOverlap(previousTranscript, result.text);
            // Whisper's Chinese training data is mostly Traditional, so "zh"
            // often yields Traditional glyphs for Mandarin audio. Normalize the
            // script after overlap removal, then apply the user's terminology.
            if (language == "zh") displayText = ChineseScript::simplify(displayText);
            if (!termEdits.empty())
                displayText = termEdits.apply(displayText, result.text);
            const bool hasText = displayText.find_first_not_of(" \r\n\t") != std::string::npos;
            std::string confirmed;
            std::string draft;
            if (closing && hasText && !result.repeated && !result.truncated) {
                // LocalAgreement stabilizes the live preview. Once the full
                // utterance is available, let it correct even a stable prefix.
                confirmed = std::move(displayText);
            } else if (!closing && hasText) {
                if (language == "zh" && result.usable() && result.score >= -.8 &&
                    result.weakFraction <= .2 && !overlapSpeech) {
                    previewClues.push_back(result.text);
                    if (previewClues.size() > 3) previewClues.pop_front();
                }
                agreement.update(std::move(displayText), !result.repeated && !result.truncated &&
                    result.score >= -.8 && result.weakFraction <= .2);
                confirmed = agreement.stable;
                draft = agreement.pending;
            } else {
                agreement.previous.clear();
            }
            if (!confirmed.empty() || !draft.empty()) {
                std::lock_guard<std::mutex> lock(mutex);
                if (active != epoch.load() || stopping.load()) return;
                captions.push_back({std::move(confirmed), end, active, -1, std::move(draft)});
                while (captions.size() > 24) captions.pop_front();
            }
            if (!closing) return;
            if (!hasText || result.repeated || result.truncated) { clearContinuity(); return; }
            if (withTimestamps) {
                if (!reviewIsRecent) reviewWindows.clear();
                if (reviewIsRecent && !followingClue.empty() && hasRetryRoom()) {
                    const auto& review = reviewWindows.front();
                    auto prompt = hintTokens;
                    prompt.insert(prompt.end(), followingClue.begin(), followingClue.end());
                    const auto revised = decode(review.audio, true, prompt, true);
                    if (active != epoch.load() || stopping.load()) return;
                    LOG_DEBUG("AI following review: score=" + std::to_string(review.score) +
                              " revised=" + std::to_string(revised.score));
                    if (revised.usable() && revised.score >= review.score + .05 &&
                        revised.weakFraction <= review.weakFraction) {
                        auto text = review.overlapText.empty() ? revised.text :
                            TranscriptBoundary::removeOverlap(review.overlapText, revised.text);
                        std::lock_guard<std::mutex> lock(mutex);
                        if (active != epoch.load() || stopping.load()) return;
                        for (const auto& caption : captions) {
                            if (caption.end != review.end || caption.epoch != active || caption.displayedAt < 0)
                                continue;
                            const auto before = TranscriptBoundary::characters(caption.text).size();
                            const auto after = TranscriptBoundary::characters(text).size();
                            const bool supported = reviewWindows.size() == 1 ||
                                TranscriptBoundary::addsSharedPair(caption.text, text, result.text);
                            if (before && after * 5 >= before * 4 && after * 4 <= before * 5 &&
                                supported && !TranscriptBoundary::equivalent(caption.text, text)) {
                                correction = {std::move(text), end};
                                reviewWindows.pop_front();
                            }
                            break;
                        }
                    }
                }
                reviewWindows.push_back({std::move(input), overlapSpeech ? previousTranscript : std::string{},
                                         end, result.score, result.weakFraction});
                while (reviewWindows.size() > 2) reviewWindows.pop_front();
            }
            previousTranscript = result.text;
            agreement.reset(); lastRecognition = 0;
            previewClues.clear();
            // Raw PCM avoids applying automatic gain twice to the overlap.
            // At a natural pause retain only the pause, not half of its last
            // spoken word. Forced cuts need spoken lookback and text dedup.
            const size_t retained = std::min(n, retain);
            overlap.assign(samples.begin() + n - retained, samples.begin() + n);
            overlapSpeech = forced && retained > 0;
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
            // Each preview re-decodes the whole pending window, and the decode
            // thread cannot consume audio while it runs, so the cadence has to
            // leave the decoder ahead of playback. With the bundled small model
            // one refresh costs about 0.5 s, so hold previews to one per 3 s of
            // audio: the confirmed chunk boundary still fires on every pause.
            const size_t previewStep = SpeechChunker::sampleRate * 3;
            if (samples.size() >= lastRecognition + previewStep) {
                bool caughtUp;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    caughtUp = queue.empty() || queue.back().pts - expected < .35;
                }
                // Let queued audio catch up before optional preview inference.
                if (caughtUp) recognize(samples.size(), 0, false, false);
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
    impl_->correction = {};
    impl_->ready.notify_one();
}
void RealtimeSubtitles::push(const AVFrame* frame, double pts, int serial) {
    if (!frame || !std::isfinite(pts) || impl_->stopping.load()) return;
    auto copy = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* p) { av_frame_free(&p); });
    if (!copy) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->serial != serial || impl_->stopping.load()) return;
    // Leave room for the second final decode while keeping the backlog bounded.
    while (impl_->queue.size() >= 128) impl_->queue.pop_front();
    impl_->queue.push_back({std::move(copy), pts, serial, impl_->epoch.load()});
    impl_->ready.notify_one();
}
void RealtimeSubtitles::finish(int serial) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (serial == impl_->serial) { impl_->eof = true; impl_->ready.notify_one(); }
}
std::pair<std::string, std::string> RealtimeSubtitles::parts(double now, int serial, std::string* correctionText) {
    if (correctionText) correctionText->clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (serial != impl_->serial || !std::isfinite(now)) return {};
    // Use media time: pause freezes visibility, speed changes do not alter PTS.
    // Decoding can run ahead; never display captions before their source audio ends.
    Impl::Caption* latest = nullptr;
    for (auto& caption : impl_->captions)
        if (caption.epoch == impl_->epoch.load() && now >= caption.end - .1)
            latest = &caption;
    if (!latest) return {};
    auto& correction = impl_->correction;
    if (!correction.text.empty() && now >= correction.availableAt) {
        if (now > correction.availableAt + 2 || latest->end > correction.availableAt + 1)
            correction = {};
        else {
            if (correction.displayedAt < 0) correction.displayedAt = now;
            if (now - correction.displayedAt >= 1)
                correction = {};
            else if (correctionText)
                *correctionText = correction.text;
        }
    }
    // Inference can finish seconds after the source chunk. Give the newest
    // eligible caption its reading time from first display, not from audio end.
    // Media time still freezes expiry while paused; a new result replaces it.
    if (latest->displayedAt < 0) {
        latest->displayedAt = now;
        LOG_DEBUG("AI display: media=" + std::to_string(now) + " end=" + std::to_string(latest->end));
    }
    return now < latest->displayedAt + 6 ? std::make_pair(latest->text, latest->pending) : std::pair<std::string, std::string>{};
}
std::string RealtimeSubtitles::text(double now, int serial) {
    const auto value = parts(now, serial);
    return value.first + value.second;
}
std::string RealtimeSubtitles::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}
}
