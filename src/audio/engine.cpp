// ghost_story — src/audio/engine.cpp
#include "audio/engine.hpp"

#include "log/logger.hpp"
#include "music/metronome.hpp"

#include <chrono>
#include <ctime>
#include <vector>

namespace ghost_story::audio {

AudioEngine::AudioEngine() { transport_.set_sample_rate(48000); }

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(BackendConfig const& cfg, std::string& reason) {
    stop();
    transport_.set_sample_rate(cfg.sample_rate);
    sr_ = cfg.sample_rate;
    backends_ = make_backends();
    for (auto& b : backends_) {
        if (!b->available()) {
            reason += std::string{b->name()};
            reason += ": unavailable; ";
            continue;
        }
        std::string why;
        if (!b->open(cfg, why)) {
            reason += std::string{b->name()};
            reason += ": ";
            reason += why;
            reason += "; ";
            continue;
        }
        b->set_render(&AudioEngine::render_trampoline, this);
        b->set_state_fn(&AudioEngine::state_trampoline, this);
        if (!b->start()) {
            reason += std::string{b->name()};
            reason += ": failed to start; ";
            b->close();
            continue;
        }
        active_ = b.get();
        log::info("audio", "started backend {} ({} Hz, {} frames)",
                  b->name(), cfg.sample_rate, cfg.buffer_size);
        return true;
    }
    reason = "no audio backend available: " + reason;
    return false;
}

void AudioEngine::stop() noexcept {
    if (active_ != nullptr) {
        active_->stop();
        active_->close();
        active_ = nullptr;
    }
    backends_.clear();
}

bool AudioEngine::is_running() const noexcept {
    return active_ != nullptr && active_->is_running();
}

std::string AudioEngine::backend_name() const {
    return active_ != nullptr ? active_->name() : "(none)";
}

CallbackStats AudioEngine::stats() const noexcept {
    return active_ != nullptr ? active_->stats() : CallbackStats{};
}

std::uint64_t AudioEngine::transport_position() const noexcept {
    // Single u64 read is atomic enough for display; RT writes are plain
    // stores inside the callback (single writer). Formally relaxed-racy
    // for exactness — bench/doctor uses this only for diagnostics, the UI
    // for the playhead (sub-sample jitter acceptable there).
    return transport_.position();
}

void AudioEngine::render_trampoline(Sample* l, Sample* r, std::uint32_t f,
                                    void* user) noexcept {
    static_cast<AudioEngine*>(user)->render(l, r, f);
}

void AudioEngine::state_trampoline(StreamState s,
                                   void* /*user*/) noexcept {
    // Non-RT thread (backend stream thread). Log only.
    switch (s) {
    case StreamState::Xrun:
        log::warn("audio", "xrun reported by backend");
        break;
    case StreamState::Error:
        log::error("audio", "audio stream error");
        break;
    default: break;
    }
}

void AudioEngine::set_master(
    void (*fn)(Sample*, Sample*, std::uint32_t, void*), void* user) noexcept {
    master_user_ = user;
    master_fn_ = fn;
}

void AudioEngine::render(Sample* left, Sample* right,
                         std::uint32_t frames) noexcept {
    // RT callback: no allocation, no locks, no logging. Boring on purpose.
    // Timing: two clock_gettime reads (vDSO, no allocation) when bench is
    // recording; nearly free otherwise (branch on relaxed atomic).
    struct timespec ts0, ts1;
    bool const record = bench_recording_.load(std::memory_order_relaxed);
    if (record) clock_gettime(CLOCK_MONOTONIC, &ts0);

    for (std::uint32_t i = 0; i < frames; ++i) {
        left[i] = 0.0f;
        right[i] = 0.0f;
    }
    if (master_fn_ != nullptr) {
        master_fn_(left, right, frames, master_user_);
    }
    transport_.advance(frames);
    music::Transport::Click clicks[8];
    int const n = transport_.clicks_in(frames, clicks, 8);
    float const gain = ctrl_.metronome_gain.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        music::render_click(left, frames, clicks[i].offset, clicks[i].accent,
                            gain, sr_);
        music::render_click(right, frames, clicks[i].offset, clicks[i].accent,
                            gain, sr_);
    }

    if (record) {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        std::uint64_t const ns = static_cast<std::uint64_t>(
            (ts1.tv_sec - ts0.tv_sec) * 1000000000LL +
            (ts1.tv_nsec - ts0.tv_nsec));
        TimingSample s;
        s.start_us = static_cast<std::uint64_t>(ts0.tv_sec) * 1000000ULL +
                     static_cast<std::uint64_t>(ts0.tv_nsec) / 1000ULL;
        s.dur_us = static_cast<std::uint32_t>(ns / 1000);
        if (!timing_q_.push(s)) {
            // Ring full: drop (bench reads fast enough).
        }
    }
}

bool AudioEngine::bench_start_record() noexcept {
    while (timing_q_.pop(timing_scratch_)) {}  // drain
    bool expected = false;
    return bench_recording_.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel);
}

bool AudioEngine::bench_stop_record() noexcept {
    return bench_recording_.exchange(false, std::memory_order_acq_rel);
}

std::size_t AudioEngine::bench_drain(TimingSample* out,
                                     std::size_t max) noexcept {
    std::size_t n = 0;
    while (n < max && timing_q_.pop(out[n])) ++n;
    return n;
}

}  // namespace ghost_story::audio
