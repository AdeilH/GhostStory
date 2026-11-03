// addu94 — src/audio/fx.cpp
//
// FX chain DSP implementing the contract in fx.hpp.
//
// Formulations:
//   - EQ: RBJ audio-EQ-cookbook biquads (low shelf @ 250 Hz, peaking
//     @ 1 kHz Q=0.9, high shelf @ 4 kHz), shelf slope S = 1. Coefficients
//     are computed in double on the control thread, normalized by a0, and
//     stored as float; a band within 1e-9 dB of 0 dB is hard-set to the
//     identity so a flat EQ is a bit-exact passthrough.
//   - Biquad filtering: transposed direct form II per channel.
//   - Delay: two independent (L/R) fixed-capacity ring buffers, capacity
//     2 s, allocated once in set_sample_rate(). Readable length is
//     delay_beats * (60/bpm) * sr samples, clamped to the ring. Feedback
//     is per-channel (input + feedback * delayed written back); the mix
//     adds the delayed tap. No interpolation: the length only changes on
//     the control thread.
//   - Chorus: fixed 20 ms ring per channel; two taps per channel read with
//     linear interpolation around a 10 ms center, modulated by a sine LFO
//     (taps in quadrature; the right channel runs 90° ahead for width).
//     Chorus depth is clamped to 8 ms so the modulated delay stays inside
//     the ring.
//   - Soft clip: y = tanh(x * drive) / tanh(drive) for drive above the
//     bypass threshold; exactly at (or below) drive == 1 the stage is a
//     bit-exact bypass (unity drive == passthrough, tested). Normalizing
//     by tanh(drive) makes y(±1) = ±1 for every drive, so the clip ceiling
//     stays at ~1.0.
//
// RT contract: process() touches only pre-allocated state and per-sample
// std::sin/std::tanh calls — no allocation, locks, logging or exceptions.

#include "audio/fx.hpp"

#include <cmath>

namespace addu94::audio {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kDelayMaxSec = 2.0;
constexpr double kChorusMaxSec = 0.020;
constexpr double kChorusBaseMs = 10.0;
constexpr double kEqLowHz = 250.0;
constexpr double kEqMidHz = 1000.0;
constexpr double kEqHighHz = 4000.0;
// At or below this the soft clip is a bit-exact bypass (drive == 1 is off).
constexpr float kDriveBypass = 1.0005f;

inline float clampf(float x, float lo, float hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

/// RBJ cookbook biquad, unnormalized. kind: 0 = low shelf, 1 = peaking,
/// 2 = high shelf (shelf slope S = 1).
struct BiquadRaw {
    double b0, b1, b2, a0, a1, a2;
};

BiquadRaw rbj_make(int kind, double f0, double gain_db, double sr,
                   double q) noexcept {
    double const a = std::pow(10.0, gain_db / 40.0);
    double const w0 = kTwoPi * f0 / sr;
    double const cw = std::cos(w0);
    double const sw = std::sin(w0);
    BiquadRaw c{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    if (kind == 1) {  // peaking
        double const alpha = sw / (2.0 * q);
        c.b0 = 1.0 + alpha * a;
        c.b1 = -2.0 * cw;
        c.b2 = 1.0 - alpha * a;
        c.a0 = 1.0 + alpha / a;
        c.a1 = -2.0 * cw;
        c.a2 = 1.0 - alpha / a;
    } else {  // shelves, S = 1  =>  alpha = sw/2 * sqrt(2)
        double const alpha = 0.5 * sw * 1.41421356237309504880;
        double const k = 2.0 * std::sqrt(a) * alpha;
        if (kind == 0) {  // low shelf
            c.b0 = a * ((a + 1.0) - (a - 1.0) * cw + k);
            c.b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
            c.b2 = a * ((a + 1.0) - (a - 1.0) * cw - k);
            c.a0 = (a + 1.0) + (a - 1.0) * cw + k;
            c.a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
            c.a2 = (a + 1.0) + (a - 1.0) * cw - k;
        } else {  // high shelf
            c.b0 = a * ((a + 1.0) + (a - 1.0) * cw + k);
            c.b1 = 2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
            c.b2 = a * ((a + 1.0) + (a - 1.0) * cw - k);
            c.a0 = (a + 1.0) - (a - 1.0) * cw + k;
            c.a1 = -2.0 * ((a - 1.0) - (a + 1.0) * cw);
            c.a2 = (a + 1.0) - (a - 1.0) * cw - k;
        }
    }
    return c;
}

/// Fractional ring read with linear interpolation. `d` is the delay in
/// samples back from the write index, clamped to [1, cap-2].
inline float ring_read(std::vector<float> const& ring, std::uint32_t cap,
                       std::uint32_t w, double d) noexcept {
    if (d < 1.0) {
        d = 1.0;
    } else if (d > static_cast<double>(cap) - 2.0) {
        d = static_cast<double>(cap) - 2.0;
    }
    double idx = static_cast<double>(w) - d;
    if (idx < 0.0) {
        idx += static_cast<double>(cap);
    }
    double const frac = idx - std::floor(idx);
    auto const i0 = static_cast<std::uint32_t>(std::floor(idx));
    std::uint32_t const i1 = i0 + 1 == cap ? 0u : i0 + 1;
    float const v0 = ring[i0];
    float const v1 = ring[i1];
    return v0 + (v1 - v0) * static_cast<float>(frac);
}

}  // namespace

float FxChain::tick_(Coefs const& c, State& s, float x) noexcept {
    float const y = c.b0 * x + s.s1;
    s.s1 = c.b1 * x - c.a1 * y + s.s2;
    s.s2 = c.b2 * x - c.a2 * y;
    return y;
}

void FxChain::recalc_() noexcept {
    // EQ bands: identity when the gain is (numerically) 0 dB so a flat EQ
    // is a bit-exact passthrough.
    auto set_band = [this](Coefs& c, int kind, double f0, float gain_db,
                           double q) {
        if (std::fabs(static_cast<double>(gain_db)) < 1.0e-9) {
            c = Coefs{};
            return;
        }
        BiquadRaw const r = rbj_make(kind, f0, static_cast<double>(gain_db),
                                     sr_, q);
        c.b0 = static_cast<float>(r.b0 / r.a0);
        c.b1 = static_cast<float>(r.b1 / r.a0);
        c.b2 = static_cast<float>(r.b2 / r.a0);
        c.a1 = static_cast<float>(r.a1 / r.a0);
        c.a2 = static_cast<float>(r.a2 / r.a0);
    };
    set_band(low_, 0, kEqLowHz, p_.eq_low_db, 0.7071067811865476);
    set_band(mid_, 1, kEqMidHz, p_.eq_mid_db, 0.9);
    set_band(high_, 2, kEqHighHz, p_.eq_high_db, 0.7071067811865476);

    // Soft clip: precompute the tanh normalization on the control thread.
    drive_ = clampf(p_.drive, 1.0f, 8.0f);
    drive_norm_ = drive_ > kDriveBypass
                      ? static_cast<float>(1.0 / std::tanh(static_cast<double>(drive_)))
                      : 1.0f;

    // Delay length: beats -> seconds -> samples, clamped to the ring.
    double const sec =
        static_cast<double>(clampf(p_.delay_beats, 1.0f / 16.0f, 1.0f)) *
        (60.0 / bpm_);
    double samples = sec * sr_;
    if (samples > static_cast<double>(delay_cap_)) {
        samples = static_cast<double>(delay_cap_);
    }
    if (samples < 1.0) {
        samples = 1.0;
    }
    delay_len_ = static_cast<std::uint32_t>(samples + 0.5);
}

void FxChain::set_params(FxParams const& p) noexcept {
    p_ = p;
    p_.eq_low_db = clampf(p_.eq_low_db, -18.0f, 18.0f);
    p_.eq_mid_db = clampf(p_.eq_mid_db, -18.0f, 18.0f);
    p_.eq_high_db = clampf(p_.eq_high_db, -18.0f, 18.0f);
    p_.delay_beats = clampf(p_.delay_beats, 1.0f / 16.0f, 1.0f);
    p_.delay_feedback = clampf(p_.delay_feedback, 0.0f, 0.9f);
    p_.delay_mix = clampf(p_.delay_mix, 0.0f, 1.0f);
    // Keep base + depth inside the 20 ms chorus ring (base is 10 ms).
    p_.chorus_depth_ms = clampf(p_.chorus_depth_ms, 0.0f, 8.0f);
    p_.chorus_rate_hz = clampf(p_.chorus_rate_hz, 0.01f, 10.0f);
    p_.chorus_mix = clampf(p_.chorus_mix, 0.0f, 1.0f);
    p_.drive = clampf(p_.drive, 1.0f, 8.0f);
    recalc_();
}

void FxChain::set_sample_rate(double sr) noexcept {
    sr_ = (sr >= 8000.0 && sr <= 192000.0) ? sr : 48000.0;
    // One-time allocation (control thread, before audio starts). The rings
    // keep their capacity afterwards; process() never resizes.
    delay_cap_ = static_cast<std::uint32_t>(sr_ * kDelayMaxSec + 0.5);
    delay_l_.assign(delay_cap_, 0.0f);
    delay_r_.assign(delay_cap_, 0.0f);
    delay_w_ = 0;
    cho_cap_ = static_cast<std::uint32_t>(sr_ * kChorusMaxSec + 0.5) + 4u;
    cho_l_.assign(cho_cap_, 0.0f);
    cho_r_.assign(cho_cap_, 0.0f);
    cho_w_ = 0;
    cho_phase_ = 0.0;
    recalc_();
}

void FxChain::set_tempo(double bpm) noexcept {
    bpm_ = (bpm >= 20.0 && bpm <= 333.0) ? bpm : 120.0;
    recalc_();  // delay length only — control thread
}

void FxChain::process(float* left, float* right,
                      std::uint32_t frames) noexcept {
    if (left == nullptr || right == nullptr) {
        return;
    }
    bool const delay_active = p_.delay_on && delay_cap_ > 0 && delay_len_ > 0;
    bool const chorus_active = p_.chorus_on && cho_cap_ > 0;
    float const d_mix = p_.delay_mix;
    float const d_fb = p_.delay_feedback;
    float const c_mix = p_.chorus_mix;
    float const c_depth = p_.chorus_depth_ms;
    double const c_inc = static_cast<double>(p_.chorus_rate_hz) / sr_;

    for (std::uint32_t i = 0; i < frames; ++i) {
        float l = left[i];
        float r = right[i];

        // --- EQ: low shelf -> peaking -> high shelf, per channel ---
        l = tick_(low_, low_s_[0], l);
        l = tick_(mid_, mid_s_[0], l);
        l = tick_(high_, high_s_[0], l);
        r = tick_(low_, low_s_[1], r);
        r = tick_(mid_, mid_s_[1], r);
        r = tick_(high_, high_s_[1], r);

        // --- Tempo-synced stereo delay (independent L/R lines) ---
        if (delay_active) {
            std::uint32_t const rp =
                (delay_w_ + delay_cap_ - delay_len_) % delay_cap_;
            float const dl = delay_l_[rp];
            float const dr = delay_r_[rp];
            delay_l_[delay_w_] = l + d_fb * dl;
            delay_r_[delay_w_] = r + d_fb * dr;
            delay_w_ = delay_w_ + 1u == delay_cap_ ? 0u : delay_w_ + 1u;
            l += d_mix * dl;
            r += d_mix * dr;
        }

        // --- Chorus: 2 interpolated taps per channel, sine LFO ---
        if (chorus_active) {
            double const ph = cho_phase_;
            // Tap delays in ms around a 10 ms center; L taps at phases
            // 0/0.5, R taps 90° ahead (0.25/0.75) for stereo width.
            double const d0 = kChorusBaseMs +
                              static_cast<double>(c_depth) * std::sin(kTwoPi * ph);
            double const d1 = kChorusBaseMs -
                              static_cast<double>(c_depth) * std::sin(kTwoPi * ph);
            double const d2 = kChorusBaseMs +
                              static_cast<double>(c_depth) *
                                  std::sin(kTwoPi * (ph + 0.25));
            double const d3 = kChorusBaseMs -
                              static_cast<double>(c_depth) *
                                  std::sin(kTwoPi * (ph + 0.25));
            double const ds = static_cast<double>(sr_) / 1000.0;
            float const tap_l = 0.5f * (ring_read(cho_l_, cho_cap_, cho_w_,
                                                  d0 * ds) +
                                        ring_read(cho_l_, cho_cap_, cho_w_,
                                                  d1 * ds));
            float const tap_r = 0.5f * (ring_read(cho_r_, cho_cap_, cho_w_,
                                                  d2 * ds) +
                                        ring_read(cho_r_, cho_cap_, cho_w_,
                                                  d3 * ds));
            float const in_l = l;  // pre-chorus input feeds the ring
            float const in_r = r;
            l += c_mix * tap_l;
            r += c_mix * tap_r;
            cho_l_[cho_w_] = in_l;
            cho_r_[cho_w_] = in_r;
            cho_w_ = cho_w_ + 1u == cho_cap_ ? 0u : cho_w_ + 1u;
            cho_phase_ += c_inc;
            if (cho_phase_ >= 1.0) {
                cho_phase_ -= 1.0;
            }
        }

        // --- Soft clip (tanh drive); drive == 1 is an exact bypass ---
        if (drive_ > kDriveBypass) {
            l = std::tanh(l * drive_) * drive_norm_;
            r = std::tanh(r * drive_) * drive_norm_;
        }

        left[i] = l;
        right[i] = r;
    }
}

}  // namespace addu94::audio
