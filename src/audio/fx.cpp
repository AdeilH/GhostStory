// ghost_story — src/audio/fx.cpp
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
//   - Compressor: per-channel envelope follower with asymmetric one-pole
//     smoothing (fast attack, slow release); static gain curve
//     g = (T + (env - T)/R) / env above the threshold, applied per sample
//     from the smoothed envelope, then makeup gain.
//   - Bitcrusher: bit-depth quantize y = round(x*q)/q (q = 2^(bits-1))
//     plus sample-hold: the output refreshes only every `downsample`
//     samples (integer hold counter).
//   - Drive: soft = tanh (y = tanh(x*drive)/tanh(drive), ceiling ~1),
//     hard = clamp(x*drive, ±1), fold = triangle foldback (mirrors the
//     signal back into ±1, fmod-based). At/below drive == 1 the stage is a
//     bit-exact bypass for every shape (unity == passthrough, tested).
//   - Delay: two independent (L/R) fixed-capacity ring buffers, capacity
//     2 s, allocated once in set_sample_rate(). Readable length is
//     delay_beats * (60/bpm) * sr samples, clamped to the ring. Feedback
//     is per-channel (input + feedback * delayed written back); the mix
//     adds the delayed tap. No interpolation: the length only changes on
//     a set_params/set_tempo call (rare; the echo may step once).
//   - Chorus: fixed 20 ms ring per channel; two taps per channel read with
//     linear interpolation around a 10 ms center, modulated by a sine LFO
//     (taps in quadrature; the right channel runs 90° ahead for width).
//     Chorus depth is clamped to 8 ms so the modulated delay stays inside
//     the ring.
//   - Flanger: fixed 5 ms ring per channel; one interpolated tap swept
//     2.5 ms ± depth (depth clamped to 2.4 ms) by a sine LFO, with the tap
//     scaled by feedback written back. Mix adds the tap to the dry signal
//     (same convention as chorus/delay).
//   - Tremolo/autopan: per-channel gain g = 1 - depth*0.5*(1 + sin(phase));
//     the right channel runs 90° ahead, so at depth 1 the image pans while
//     at any depth it also breathes.
//   - Reverb: standalone Schroeder (see Reverb below), applied as the LAST
//     chain stage over the whole block (dry stays full: out = dry + m*wet).
//
// RT contract: process() touches only pre-allocated state and per-sample
// std::sin/std::tanh/std::fmod calls — no allocation, locks, logging or
// exceptions. set_params/set_tempo are copy + clamp + pure math and are
// RT-callable (the play engine applies queued updates at buffer
// boundaries); set_sample_rate allocates and must stay pre-RT.

#include "audio/fx.hpp"

#include <cmath>

namespace ghost_story::audio {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kDelayMaxSec = 2.0;
constexpr double kChorusMaxSec = 0.020;
constexpr double kChorusBaseMs = 10.0;
constexpr double kFlangerMaxSec = 0.005;
constexpr double kFlangerBaseMs = 2.5;
constexpr double kEqLowHz = 250.0;
constexpr double kEqMidHz = 1000.0;
constexpr double kEqHighHz = 4000.0;
// At or below this the waveshaper is a bit-exact bypass (drive == 1 is off).
constexpr float kDriveBypass = 1.0005f;

inline float clampf(float x, float lo, float hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

/// Triangle foldback: mirrors the input back into [-1, 1] (piecewise
/// linear, fmod-based). Input 0.5 -> 0.5, 1.5 -> 0.5, 2.5 -> -0.5,
/// 3.5 -> -0.5, 4.5 -> 0.5 — folds back every 4 units of drive, bounded.
inline float foldback(float v) noexcept {
    float const t = std::fmod(std::fabs(v), 4.0f);
    return t < 1.0f ? t : (t < 3.0f ? 2.0f - t : t - 4.0f);
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

// ------------------------------------------------------------------ Reverb

void Reverb::set_params(ReverbParams const& p) noexcept {
    p_ = p;
    p_.mix = clampf(p_.mix, 0.0f, 1.0f);
    p_.decay = clampf(p_.decay, 0.0f, 1.0f);
    // decay 0..1 -> comb feedback 0.55..0.95 (Freeverb-style room mapping).
    comb_fb_ = 0.55f + 0.40f * p_.decay;
}

void Reverb::set_sample_rate(double sr) noexcept {
    sr_ = (sr >= 8000.0 && sr <= 192000.0) ? sr : 48000.0;
    double const sr_d = static_cast<double>(sr_);
    double const spread_ms = 2.1;  // R combs run ahead of L for width
    for (int c = 0; c < kCombs; ++c) {
        std::uint32_t const cap = static_cast<std::uint32_t>(
            sr_d * (kCombMs[static_cast<std::size_t>(c)] + spread_ms) /
                1000.0 +
            8.0);
        comb_l_[static_cast<std::size_t>(c)].assign(cap, 0.0f);
        comb_r_[static_cast<std::size_t>(c)].assign(cap, 0.0f);
        // L reads at the nominal delay; R reads spread_ms further (the
        // capacity includes the margin, so both stay inside the ring).
        comb_len_l_[static_cast<std::size_t>(c)] =
            static_cast<std::uint32_t>(
                sr_d * kCombMs[static_cast<std::size_t>(c)] / 1000.0 + 0.5);
        comb_len_r_[static_cast<std::size_t>(c)] =
            comb_len_l_[static_cast<std::size_t>(c)] +
            static_cast<std::uint32_t>(sr_d * spread_ms / 1000.0 + 0.5);
    }
    for (int a = 0; a < kAps; ++a) {
        std::uint32_t const cap = static_cast<std::uint32_t>(
            sr_d * kApMs[static_cast<std::size_t>(a)] / 1000.0 + 8.0);
        ap_l_[static_cast<std::size_t>(a)].assign(cap, 0.0f);
        ap_r_[static_cast<std::size_t>(a)].assign(cap, 0.0f);
        ap_len_[static_cast<std::size_t>(a)] = static_cast<std::uint32_t>(
            sr_d * kApMs[static_cast<std::size_t>(a)] / 1000.0 + 0.5);
    }
    clear();
}

void Reverb::clear() noexcept {
    for (int c = 0; c < kCombs; ++c) {
        comb_w_[static_cast<std::size_t>(c)] = 0;
        auto& l = comb_l_[static_cast<std::size_t>(c)];
        auto& r = comb_r_[static_cast<std::size_t>(c)];
        for (std::size_t i = 0; i < l.size(); ++i) {
            l[i] = 0.f;
        }
        for (std::size_t i = 0; i < r.size(); ++i) {
            r[i] = 0.f;
        }
    }
    for (int a = 0; a < kAps; ++a) {
        ap_w_[static_cast<std::size_t>(a)] = 0;
        auto& l = ap_l_[static_cast<std::size_t>(a)];
        auto& r = ap_r_[static_cast<std::size_t>(a)];
        for (std::size_t i = 0; i < l.size(); ++i) {
            l[i] = 0.f;
        }
        for (std::size_t i = 0; i < r.size(); ++i) {
            r[i] = 0.f;
        }
    }
}

void Reverb::process(float* left, float* right,
                     std::uint32_t frames) noexcept {
    if (left == nullptr || right == nullptr) {
        return;
    }
    // Pre-init guard: rings exist only after set_sample_rate (the % cap
    // below would be a division by zero). Silent bypass is the honest
    // pre-init state; the play engine always inits before audio starts.
    if (comb_l_[0].empty() || ap_l_[0].empty()) {
        return;
    }
    float const fb = comb_fb_;
    float const ap_g = 0.5f;
    for (std::uint32_t i = 0; i < frames; ++i) {
        float const l = left[i];
        float const r = right[i];
        // 4 parallel feedback combs per channel (y = x + g*y[n-D]), the L
        // and R taps read at different delays for stereo width; summed /4.
        float cl = 0.f;
        float cr = 0.f;
        for (int c = 0; c < kCombs; ++c) {
            auto& rl = comb_l_[static_cast<std::size_t>(c)];
            auto& rr = comb_r_[static_cast<std::size_t>(c)];
            std::uint32_t const cap = static_cast<std::uint32_t>(rl.size());
            std::uint32_t const w = comb_w_[static_cast<std::size_t>(c)];
            std::uint32_t const pl =
                (w + cap - comb_len_l_[static_cast<std::size_t>(c)]) % cap;
            std::uint32_t const pr =
                (w + cap - comb_len_r_[static_cast<std::size_t>(c)]) % cap;
            float const vl = rl[pl];
            float const vr = rr[pr];
            rl[w] = l + fb * vl;
            rr[w] = r + fb * vr;
            comb_w_[static_cast<std::size_t>(c)] =
                w + 1u == cap ? 0u : w + 1u;
            cl += vl;
            cr += vr;
        }
        float wl = cl * 0.25f;
        float wr = cr * 0.25f;
        // 2 series allpass per channel (Freeverb formulation:
        // out = -in + buf;  buf_new = in + 0.5*buf).
        for (int a = 0; a < kAps; ++a) {
            auto& rl = ap_l_[static_cast<std::size_t>(a)];
            auto& rr = ap_r_[static_cast<std::size_t>(a)];
            std::uint32_t const cap = static_cast<std::uint32_t>(rl.size());
            std::uint32_t const w = ap_w_[static_cast<std::size_t>(a)];
            std::uint32_t const p =
                (w + cap - ap_len_[static_cast<std::size_t>(a)]) % cap;
            float const vl = rl[p];
            float const vr = rr[p];
            float const ol = -wl + vl;
            float const or_ = -wr + vr;
            rl[w] = wl + ap_g * vl;
            rr[w] = wr + ap_g * vr;
            ap_w_[static_cast<std::size_t>(a)] =
                w + 1u == cap ? 0u : w + 1u;
            wl = ol;
            wr = or_;
        }
        left[i] += p_.mix * wl;
        right[i] += p_.mix * wr;
    }
}

// ----------------------------------------------------------------- FxChain

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

    // Soft shape only: precompute the tanh normalization. Hard/fold shapes
    // are self-normalizing (bounded by construction).
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
    p_.comp_threshold = clampf(p_.comp_threshold, 0.02f, 1.0f);
    p_.comp_ratio = clampf(p_.comp_ratio, 1.0f, 20.0f);
    p_.comp_makeup = clampf(p_.comp_makeup, 0.5f, 4.0f);
    p_.crush_bits = clampf(p_.crush_bits, 2.0f, 16.0f);
    p_.crush_downsample = clampf(p_.crush_downsample, 1.0f, 16.0f);
    p_.drive = clampf(p_.drive, 1.0f, 8.0f);
    p_.delay_beats = clampf(p_.delay_beats, 1.0f / 16.0f, 1.0f);
    p_.delay_feedback = clampf(p_.delay_feedback, 0.0f, 0.9f);
    p_.delay_mix = clampf(p_.delay_mix, 0.0f, 1.0f);
    // Keep base + depth inside the 20 ms chorus ring (base is 10 ms).
    p_.chorus_depth_ms = clampf(p_.chorus_depth_ms, 0.0f, 8.0f);
    p_.chorus_rate_hz = clampf(p_.chorus_rate_hz, 0.01f, 10.0f);
    p_.chorus_mix = clampf(p_.chorus_mix, 0.0f, 1.0f);
    // Keep base + depth inside the 5 ms flanger ring (base is 2.5 ms).
    p_.flanger_depth_ms = clampf(p_.flanger_depth_ms, 0.0f, 2.4f);
    p_.flanger_rate_hz = clampf(p_.flanger_rate_hz, 0.05f, 10.0f);
    p_.flanger_feedback = clampf(p_.flanger_feedback, 0.0f, 0.9f);
    p_.flanger_mix = clampf(p_.flanger_mix, 0.0f, 1.0f);
    p_.tremolo_rate_hz = clampf(p_.tremolo_rate_hz, 0.05f, 20.0f);
    p_.tremolo_depth = clampf(p_.tremolo_depth, 0.0f, 1.0f);
    p_.reverb_mix = clampf(p_.reverb_mix, 0.0f, 1.0f);
    p_.reverb_decay = clampf(p_.reverb_decay, 0.0f, 1.0f);
    ReverbParams rp;
    rp.on = p_.reverb_on;
    rp.mix = p_.reverb_mix;
    rp.decay = p_.reverb_decay;
    reverb_.set_params(rp);
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
    fla_cap_ = static_cast<std::uint32_t>(sr_ * kFlangerMaxSec + 0.5) + 8u;
    fla_l_.assign(fla_cap_, 0.0f);
    fla_r_.assign(fla_cap_, 0.0f);
    fla_w_ = 0;
    fla_phase_ = 0.0;
    comp_env_[0] = 0.f;
    comp_env_[1] = 0.f;
    crush_hold_[0] = 0.f;
    crush_hold_[1] = 0.f;
    crush_phase_ = 0;
    trem_phase_ = 0.0;
    reverb_.set_sample_rate(sr_);
    recalc_();
}

void FxChain::set_tempo(double bpm) noexcept {
    bpm_ = (bpm >= 20.0 && bpm <= 333.0) ? bpm : 120.0;
    recalc_();  // delay length only — pure math, RT-safe (see header)
}

void FxChain::process(float* left, float* right,
                      std::uint32_t frames) noexcept {
    if (left == nullptr || right == nullptr) {
        return;
    }
    bool const delay_active = p_.delay_on && delay_cap_ > 0 && delay_len_ > 0;
    bool const chorus_active = p_.chorus_on && cho_cap_ > 0;
    bool const flanger_active = p_.flanger_on && fla_cap_ > 0;
    bool const comp_active = p_.comp_on;
    bool const crush_active = p_.crush_on;
    bool const drive_active = drive_ > kDriveBypass;
    bool const tremolo_active = p_.tremolo_on;
    float const d_mix = p_.delay_mix;
    float const d_fb = p_.delay_feedback;
    float const c_mix = p_.chorus_mix;
    float const c_depth = p_.chorus_depth_ms;
    double const c_inc = static_cast<double>(p_.chorus_rate_hz) / sr_;
    float const f_mix = p_.flanger_mix;
    float const f_fb = p_.flanger_feedback;
    float const f_depth = p_.flanger_depth_ms;
    double const f_inc = static_cast<double>(p_.flanger_rate_hz) / sr_;
    double const f_ds = static_cast<double>(sr_) / 1000.0;
    float const t_depth = p_.tremolo_depth;
    double const t_inc = static_cast<double>(p_.tremolo_rate_hz) / sr_;
    // Compressor one-pole smoothing coefficients: fast attack (2 ms),
    // slow release (120 ms) — asymmetric, computed per buffer.
    float const att_c = static_cast<float>(1.0 - std::exp(-1.0 / (sr_ * 0.002)));
    float const rel_c = static_cast<float>(1.0 - std::exp(-1.0 / (sr_ * 0.120)));
    float const thr = p_.comp_threshold;
    float const rcp = 1.0f / p_.comp_ratio;
    float const makeup = p_.comp_makeup;
    float const crush_q = std::pow(2.0f, p_.crush_bits - 1.0f);
    auto const crush_hold_n = static_cast<std::uint32_t>(
        p_.crush_downsample + 0.5f);
    float const drive = drive_;

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

        // --- Compressor: envelope follower + smoothed gain curve ---
        if (comp_active) {
            for (int ch = 0; ch < 2; ++ch) {
                float const x = ch == 0 ? l : r;
                float const a = x < 0.f ? -x : x;
                float& env = comp_env_[ch];
                env += (a > env ? att_c : rel_c) * (a - env);
                float g = 1.0f;
                if (env > thr) {
                    g = (thr + (env - thr) * rcp) / env;
                }
                float const out = x * g * makeup;
                if (ch == 0) {
                    l = out;
                } else {
                    r = out;
                }
            }
        }

        // --- Bitcrusher: bit quantize + sample-hold ---
        if (crush_active) {
            if (crush_phase_ == 0) {
                float const ql = std::round(l * crush_q) / crush_q;
                float const qr = std::round(r * crush_q) / crush_q;
                crush_hold_[0] = ql < -1.f ? -1.f : (ql > 1.f ? 1.f : ql);
                crush_hold_[1] = qr < -1.f ? -1.f : (qr > 1.f ? 1.f : qr);
            }
            crush_phase_ =
                crush_phase_ + 1u >= crush_hold_n ? 0u : crush_phase_ + 1u;
            l = crush_hold_[0];
            r = crush_hold_[1];
        }

        // --- Waveshaper drive (soft/hard/fold); drive == 1 is bypass ---
        if (drive_active) {
            switch (p_.drive_shape) {
            case DriveShape::Hard:
                l = l * drive;
                l = l < -1.f ? -1.f : (l > 1.f ? 1.f : l);
                r = r * drive;
                r = r < -1.f ? -1.f : (r > 1.f ? 1.f : r);
                break;
            case DriveShape::Fold:
                l = foldback(l * drive);
                r = foldback(r * drive);
                break;
            case DriveShape::Soft:
            default:
                l = std::tanh(l * drive) * drive_norm_;
                r = std::tanh(r * drive) * drive_norm_;
                break;
            }
        }

        // --- Chorus: 2 interpolated taps per channel, sine LFO ---
        if (chorus_active) {
            double const ph = cho_phase_;
            // Tap delays in ms around a 10 ms center; L taps at phases
            // 0/0.5, R taps 90° ahead (0.25/0.75) for stereo width.
            double const s0 = std::sin(kTwoPi * ph);
            double const d0 = kChorusBaseMs + static_cast<double>(c_depth) * s0;
            double const d1 = kChorusBaseMs - static_cast<double>(c_depth) * s0;
            double const s2 = std::sin(kTwoPi * (ph + 0.25));
            double const d2 = kChorusBaseMs + static_cast<double>(c_depth) * s2;
            double const d3 = kChorusBaseMs - static_cast<double>(c_depth) * s2;
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

        // --- Flanger: swept tap with feedback (L/R share the sweep) ---
        if (flanger_active) {
            double const d =
                (kFlangerBaseMs +
                 static_cast<double>(f_depth) *
                     std::sin(kTwoPi * fla_phase_)) *
                f_ds;
            float const tap_l = ring_read(fla_l_, fla_cap_, fla_w_, d);
            float const tap_r = ring_read(fla_r_, fla_cap_, fla_w_, d);
            fla_l_[fla_w_] = l + f_fb * tap_l;
            fla_r_[fla_w_] = r + f_fb * tap_r;
            fla_w_ = fla_w_ + 1u == fla_cap_ ? 0u : fla_w_ + 1u;
            l += f_mix * tap_l;
            r += f_mix * tap_r;
            fla_phase_ += f_inc;
            if (fla_phase_ >= 1.0) {
                fla_phase_ -= 1.0;
            }
        }

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

        // --- Tremolo / autopan: R runs 90° ahead (pan movement) ---
        if (tremolo_active) {
            float const gl = 1.0f - t_depth * 0.5f *
                                       (1.0f + static_cast<float>(
                                                   std::sin(kTwoPi *
                                                            trem_phase_)));
            float const gr = 1.0f - t_depth * 0.5f *
                                       (1.0f + static_cast<float>(
                                                   std::sin(kTwoPi *
                                                            (trem_phase_ +
                                                             0.25))));
            l *= gl;
            r *= gr;
            trem_phase_ += t_inc;
            if (trem_phase_ >= 1.0) {
                trem_phase_ -= 1.0;
            }
        }

        left[i] = l;
        right[i] = r;
    }

    // --- Reverb: LAST stage, block-level (dry stays full, adds m*wet) ---
    if (p_.reverb_on) {
        reverb_.process(left, right, frames);
    }
}

}  // namespace ghost_story::audio
