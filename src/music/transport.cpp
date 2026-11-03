// addu94 — src/music/transport.cpp
#include "music/transport.hpp"

#include <cmath>

namespace addu94::music {

Transport::Transport(TransportControl& ctrl) noexcept : ctrl_(ctrl) {}

void Transport::set_sample_rate(std::uint32_t sr) noexcept {
    // Control-thread only, before audio starts (documented contract; never
    // on the RT path).
    sample_rate_ = static_cast<double>(sr);
    sr_ = sr;
}

void Transport::latch() noexcept {
    tempo_ = ctrl_.tempo_bpm.load(std::memory_order_relaxed);
    if (tempo_ < 20.0) tempo_ = 20.0;
    if (tempo_ > 333.0) tempo_ = 333.0;
    meter_num_ = ctrl_.meter_num.load(std::memory_order_relaxed);
    if (meter_num_ == 0) meter_num_ = 4;
    meter_den_ = ctrl_.meter_den.load(std::memory_order_relaxed);
    if (meter_den_ == 0) meter_den_ = 4;
    std::uint64_t const bars = ctrl_.loop_bars.load(std::memory_order_relaxed);
    if (bars == 0) {
        loop_samples_ = 0;
    } else {
        double const beats_per_bar = static_cast<double>(meter_num_) * 4.0 /
                                     static_cast<double>(meter_den_);
        double const samples =
            static_cast<double>(bars) * beats_per_bar * seconds_per_beat() *
            sample_rate_;
        loop_samples_ = static_cast<std::uint64_t>(samples + 0.5);
    }
}

void Transport::apply_state_requests() noexcept {
    // Exchange: latest request wins if the control thread queues several
    // within one buffer (documented; UI-level debouncing handles rapid
    // toggles). Cleared atomically so a request can never re-fire.
    std::uint8_t const req =
        ctrl_.state_req.exchange(TransportControl::kReqNone,
                                 std::memory_order_acq_rel);
    if (req == TransportControl::kReqNone) return;
    switch (req) {
    case TransportControl::kReqPlay: start(); break;
    case TransportControl::kReqStop: stop(); break;
    case TransportControl::kReqToggleRecord:
        if (state_ == PlayState::Recording) {
            state_ = PlayState::Playing;
        } else {
            start();
            state_ = PlayState::Recording;
        }
        break;
    default: break;
    }
}

void Transport::advance(std::uint32_t frames) noexcept {
    apply_state_requests();
    if (state_ == PlayState::Stopped) return;
    latch();
    pos_ += frames;
    if (loop_samples_ != 0 && pos_ >= loop_samples_) {
        pos_ %= loop_samples_;  // sample-exact wrap
    }
}

void Transport::start() noexcept {
    latch();
    if (state_ == PlayState::Stopped) {
        pos_ = 0;  // start from top; punch-in/overdub keeps position (M3)
    }
    state_ = PlayState::Playing;
}

void Transport::stop() noexcept { state_ = PlayState::Stopped; }

void Transport::set_recording(bool on) noexcept {
    if (on && state_ != PlayState::Stopped) state_ = PlayState::Recording;
    else if (!on && state_ == PlayState::Recording) state_ = PlayState::Playing;
}

void Transport::set_loop_bars(std::uint64_t bars) noexcept {
    ctrl_.loop_bars.store(bars, std::memory_order_relaxed);
}

double Transport::ticks() const noexcept {
    double const samples_per_tick =
        sample_rate_ * 60.0 / (tempo_ * static_cast<double>(kPPQ));
    return static_cast<double>(pos_) / samples_per_tick;
}

double Transport::beats() const noexcept {
    return ticks() / static_cast<double>(kPPQ);
}

double Transport::bars() const noexcept {
    double const beats_per_bar =
        static_cast<double>(meter_num_) * 4.0 / static_cast<double>(meter_den_);
    return beats() / beats_per_bar;
}

bool Transport::on_beat_boundary() const noexcept {
    double const samples_per_beat =
        sample_rate_ * seconds_per_beat() * 4.0 /
        static_cast<double>(meter_den_);
    double const nearest =
        std::round(static_cast<double>(pos_) / samples_per_beat) *
        samples_per_beat;
    return std::abs(static_cast<double>(pos_) - nearest) < 0.5;
}

int Transport::clicks_in(std::uint32_t frames, Click* out,
                         int max_out) const noexcept {
    if (max_out <= 0 || out == nullptr) return 0;
    if (!ctrl_.metronome_on.load(std::memory_order_relaxed)) return 0;
    if (state_ == PlayState::Stopped) return 0;

    double const spb =
        sample_rate_ * seconds_per_beat() * 4.0 / static_cast<double>(meter_den_);
    std::uint64_t const beats_per_bar = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(meter_num_) * 4.0 /
                     static_cast<double>(meter_den_)));
    if (spb <= 0.0 || beats_per_bar == 0) return 0;

    int n = 0;
    // First beat index at-or-after pos_ (round-up; then fix rounding drift).
    std::uint64_t beat = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(pos_) / spb));
    for (;;) {
        double const onset = static_cast<double>(beat) * spb;
        if (onset < static_cast<double>(pos_)) {  // rounding drift downward
            ++beat;
            continue;
        }
        auto const offset64 = static_cast<std::uint64_t>(onset + 0.5) - pos_;
        if (offset64 >= frames) break;
        if (n < max_out) {
            out[n].offset = static_cast<std::uint32_t>(offset64);
            out[n].accent = (beat % beats_per_bar) == 0;
        }
        ++n;
        ++beat;
        if (n >= max_out) break;
    }
    return n;
}

}  // namespace addu94::music
