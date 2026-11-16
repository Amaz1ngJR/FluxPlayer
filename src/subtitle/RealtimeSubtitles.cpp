#include "FluxPlayer/subtitle/RealtimeSubtitles.h"
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
    struct Caption { std::string text; double end; uint64_t epoch; };
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
        size_t analyzed = 0, quietSamples = 0;
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
        const std::string prompt = (language == "zh" ? "以下是普通话的简体中文字幕。" : "") + hints;
        auto hintTokens = tokenize(prompt);
        if (hintTokens.size() > 96) hintTokens.resize(96);
        std::vector<whisper_token> history;
        struct Candidate { std::string text; double score = -100; };
        auto recognize = [&](size_t n) {
            if (n < 6400 || active != epoch.load() || stopping.load()) return;
            double energy = 0;
            for (size_t i = 0; i < n; ++i) energy += samples[i] * samples[i];
            if (energy / n < 0.00001) { resetContext = true; return; }
            abort.epoch = active;
            auto decode = [&](bool useHistory, int beams) {
                auto tokens = hintTokens;
                if (useHistory && !resetContext) tokens.insert(tokens.end(), history.begin(), history.end());
                params.no_context = true;
                params.prompt_tokens = tokens.empty() ? nullptr : tokens.data();
                params.prompt_n_tokens = static_cast<int>(tokens.size());
                params.beam_search.beam_size = beams;
                Candidate result;
                if (whisper_full(ctx.get(), params, samples.data(), static_cast<int>(n)) != 0) {
                    if (active == epoch.load() && !stopping.load()) throw std::runtime_error("inference failed");
                    return result;
                }
                double total = 0; size_t count = 0;
                for (int i = 0; i < whisper_full_n_segments(ctx.get()); ++i) {
                    result.text += whisper_full_get_segment_text(ctx.get(), i);
                    for (int j = 0; j < whisper_full_n_tokens(ctx.get(), i); ++j) {
                        const auto token = whisper_full_get_token_data(ctx.get(), i, j);
                        if (token.id >= whisper_token_eot(ctx.get())) continue;
                        total += std::log(std::max(1e-6f, token.p)); ++count;
                    }
                }
                if (count) result.score = total / count;
                return result;
            };
            auto result = decode(true, 5);
            if (active != epoch.load() || stopping.load()) return;
            if (result.score < -0.8) {
                // One bounded retry, without recent transcript. Scores are a
                // heuristic, not calibrated accuracy or a semantic correction.
                const auto retry = decode(false, 8);
                const bool comparable = result.text.empty() ||
                    (retry.text.size() * 2 >= result.text.size() && retry.text.size() <= result.text.size() * 2);
                if (comparable && retry.score > result.score) result = retry;
            }
            if (active != epoch.load() || stopping.load()) return;
            if (result.text.find_first_not_of(" \r\n\t") == std::string::npos) { resetContext = true; return; }
            history.clear();
            resetContext = result.score < -0.8;
            if (!resetContext) {
                history = tokenize(result.text);
                if (history.size() > 96) history.erase(history.begin(), history.end() - 96);
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (active != epoch.load() || stopping.load()) return;
            captions.push_back({std::move(result.text), start + n / 16000., active});
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
                    analyzed = quietSamples = 0; resetContext = true;
                }
                if (!queue.empty()) { in = std::move(queue.front()); queue.pop_front(); }
                else if (eof) { eof = false; tail = true; }
                else continue;
            }
            if (tail) {
                recognize(samples.size()); samples.clear(); expected = -1;
                analyzed = quietSamples = 0; resetContext = true;
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
                analyzed = quietSamples = 0; resetContext = true;
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
            // Analyze 20ms blocks once. Prefer a 400ms quiet boundary after 2.5s,
            // with an 8s cap for continuous speech. Keep chunks non-overlapping so
            // the UI never repeats rolling text. This is an energy gate, not VAD.
            while (analyzed + 320 <= samples.size() && active == epoch.load() && !stopping.load()) {
                double blockEnergy = 0;
                for (size_t i = analyzed; i < analyzed + 320; ++i)
                    blockEnergy += samples[i] * samples[i];
                quietSamples = blockEnergy / 320 < 0.00001 ? quietSamples + 320 : 0;
                analyzed += 320;
                if ((analyzed >= 40000 && quietSamples >= 6400) || analyzed >= 128000) {
                    const size_t count = analyzed;
                    recognize(count);
                    samples.erase(samples.begin(), samples.begin() + count);
                    start += count / 16000.;
                    analyzed = quietSamples = 0;
                }
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
    std::string result;
    for (const auto& caption : impl_->captions)
        if (caption.epoch == impl_->epoch.load() && now >= caption.end - .1 && now < caption.end + 6)
            result = caption.text;
    return result;
}
std::string RealtimeSubtitles::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}
}
