// addu94 — src/audio/fx.hpp
//
// Per-track effect chain (spec §2.7): 3-band RBJ EQ, tempo-synced stereo
// delay, 2-tap modulated chorus, tanh soft clip. ONE header-defined
// contract; the DSP lives in fx.cpp.
//
// RT contract (mirrors synth.hpp): process() NEVER allocates, locks or
// logs. Both delay rings (2 s max) and the chorus ring (20 ms max) are
// allocated exactly once in set_sample_rate(), which the control thread
// calls before audio starts. set_params()/set_tempo() are control-thread
// only: they recompute biquad coefficients and the delay line length
// (control-thread math, never mid-buffer).
//
// Signal flow: EQ (low shelf -> peaking -> high shelf) -> delay -> chorus
// -> soft clip.
#pragma once

#include <cstdint>
#include <vector>

namespace addu94::audio {

struct FxParams {
    // 3-band EQ (RBJ biquad: low shelf / peaking / high shelf), dB -18..+18
    float eq_low_db{0.f};
    float eq_mid_db{0.f};
    float eq_high_db{0.f};
    // Tempo-synced stereo delay
    bool delay_on{false};
    float delay_beats{0.25f};    // 1/16..1 note
    float delay_feedback{0.3f};  // 0..0.9
    float delay_mix{0.25f};      // 0..1
    // Chorus (2 modulated taps)
    bool chorus_on{false};
    float chorus_depth_ms{2.5f};
    float chorus_rate_hz{0.8f};
    float chorus_mix{0.3f};
    // Soft clip (tanh drive) at chain end; 1 = bypass
    float drive{1.0f};           // 1..~4
};

class FxChain {
public:
    FxChain() noexcept = default;

    void set_params(FxParams const& p) noexcept;   // control thread
    void set_sample_rate(double sr) noexcept;      // control thread, once pre-RT
    void set_tempo(double bpm) noexcept;           // control thread (delay length)

    /// RT: in-place stereo process. No allocation, no locks, no logging.
    void process(float* left, float* right, std::uint32_t frames) noexcept;

private:
    // RBJ biquad coefficients, computed in double on the control thread,
    // stored as float. Always normalized by a0 (b0 == 1 for flat bands).
    struct Coefs {
        float b0{1.f}, b1{0.f}, b2{0.f}, a1{0.f}, a2{0.f};
    };
    struct State {
        float s1{0.f}, s2{0.f};
    };

    /// Control thread: recompute EQ coefficients, drive normalization and
    /// the delay line length from p_ / sr_ / bpm_.
    void recalc_() noexcept;

    /// One transposed-direct-form-II biquad sample (defined in fx.cpp).
    static float tick_(Coefs const& c, State& s, float x) noexcept;

    FxParams p_{};
    double sr_{48000.0};
    double bpm_{120.0};

    Coefs low_{}, mid_{}, high_{};
    State low_s_[2]{}, mid_s_[2]{}, high_s_[2]{};  // per channel [L, R]
    float drive_{1.f};        // clamped drive (> bypass threshold == active)
    float drive_norm_{1.f};   // 1 / tanh(drive_)

    std::vector<float> delay_l_, delay_r_;  // fixed 2 s max ring, pre-allocated
    std::uint32_t delay_cap_{0};
    std::uint32_t delay_len_{0};            // current musical length (samples)
    std::uint32_t delay_w_{0};

    std::vector<float> cho_l_, cho_r_;      // fixed 20 ms max ring, pre-allocated
    std::uint32_t cho_cap_{0};
    std::uint32_t cho_w_{0};
    double cho_phase_{0.0};
};

}  // namespace addu94::audio
