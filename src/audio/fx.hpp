// ghost_story — src/audio/fx.hpp
//
// Per-track effect chain: 3-band RBJ EQ, compressor, bitcrusher, waveshaper
// drive (soft/hard/fold), tempo-synced stereo delay, 2-tap modulated chorus,
// flanger, tremolo/autopan, Schroeder reverb.
//
// ONE header-defined contract; the DSP lives in fx.cpp.
//
// RT contract (mirrors synth.hpp): process() NEVER allocates, locks or
// logs. All rings (delay 2 s max, chorus/flanger < 20 ms, reverb < 50 ms)
// are allocated exactly once in set_sample_rate(), which the control thread
// calls before audio starts. set_params()/set_tempo() are pure math (copy +
// clamp + coefficient/length recompute, zero allocation) — they are safe to
// call from the audio thread too, which is how the play engine applies
// queued parameter updates at buffer boundaries. set_sample_rate() remains
// control-thread only (it allocates).
//
// Signal flow: EQ (low shelf -> peaking -> high shelf) -> compressor ->
// bitcrusher -> drive (soft/hard/fold) -> chorus -> flanger -> delay ->
// tremolo -> reverb. Every block gates on its on/mix param and defaults to
// a bit-exact bypass when off: a default-constructed FxParams produces a
// bit-identical passthrough (tested) — the offline render path depends on
// that for its determinism contract.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ghost_story::audio {

/// Waveshaper character for the drive stage (chain order: EQ -> comp ->
/// crush -> drive -> ...).
enum class DriveShape : std::uint8_t { Soft, Hard, Fold };

/// Standalone Schroeder reverb (4 parallel combs + 2 series allpass per
/// channel, slight L/R delay spread for width). Composed inside FxChain;
/// kept standalone so a shared send bus can reuse the identical DSP later.
/// Same RT contract as FxChain: allocate only in set_sample_rate (once,
/// pre-RT); set_params is alloc-free math (RT-callable).
struct ReverbParams {
    bool on{false};
    float mix{0.25f};    // 0..1 wet amount (dry stays full: out = dry + m*wet)
    float decay{0.6f};   // 0..1 -> comb feedback 0.55..0.95
};

class Reverb {
public:
    Reverb() noexcept = default;

    void set_params(ReverbParams const& p) noexcept;  // alloc-free, RT-safe
    void set_sample_rate(double sr) noexcept;         // control thread, once pre-RT
    /// Reset comb/allpass memory and phase-free state (control thread or RT).
    void clear() noexcept;

    /// RT: in-place stereo process. No allocation, no locks, no logging.
    void process(float* left, float* right, std::uint32_t frames) noexcept;

private:
    static constexpr int kCombs = 4;
    static constexpr int kAps = 2;
    // Comb delays in ms (L channel; R runs +2.1 ms ahead for stereo width).
    static constexpr std::array<double, kCombs> kCombMs{29.66, 37.27, 41.36,
                                                        43.79};
    static constexpr std::array<double, kAps> kApMs{5.05, 1.72};

    std::array<std::vector<float>, kCombs> comb_l_{}, comb_r_{};
    std::array<std::vector<float>, kAps> ap_l_{}, ap_r_{};
    // Read delays: R combs run spread_ms ahead of L for stereo width.
    std::array<std::uint32_t, kCombs> comb_len_l_{}, comb_len_r_{};
    std::array<std::uint32_t, kCombs> comb_w_{};
    std::array<std::uint32_t, kAps> ap_len_{}, ap_w_{};
    float comb_fb_{0.8f};
    double sr_{48000.0};
    ReverbParams p_{};
};

struct FxParams {
    // 3-band EQ (RBJ biquad: low shelf / peaking / high shelf), dB -18..+18
    float eq_low_db{0.f};
    float eq_mid_db{0.f};
    float eq_high_db{0.f};
    // Compressor (envelope follower, per channel)
    bool comp_on{false};
    float comp_threshold{0.30f};  // linear amplitude 0.02..1
    float comp_ratio{2.0f};       // 1..20
    float comp_makeup{1.0f};      // 0.5..4 (1 = no makeup)
    // Bitcrusher (bit quantize + sample-hold)
    bool crush_on{false};
    float crush_bits{8.0f};        // 2..16
    float crush_downsample{1.0f};  // 1..16 hold
    // Drive: shape + tanh/hard/fold amount; 1 = bypass
    DriveShape drive_shape{DriveShape::Soft};
    float drive{1.0f};             // 1..8
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
    // Flanger (modulated short delay with feedback)
    bool flanger_on{false};
    float flanger_rate_hz{0.30f};
    float flanger_depth_ms{2.0f};  // 0..2.4 (base 2.5 ms)
    float flanger_feedback{0.5f};  // 0..0.9
    float flanger_mix{0.5f};       // 0..1
    // Tremolo / autopan (stereo phase offset = pan movement at depth 1)
    bool tremolo_on{false};
    float tremolo_rate_hz{4.0f};   // 0.05..20
    float tremolo_depth{0.5f};     // 0..1
    // Reverb (Schroeder insert; dry stays full)
    bool reverb_on{false};
    float reverb_mix{0.25f};       // 0..1
    float reverb_decay{0.60f};     // 0..1
};

class FxChain {
public:
    FxChain() noexcept = default;

    void set_params(FxParams const& p) noexcept;   // alloc-free, RT-safe
    void set_sample_rate(double sr) noexcept;      // control thread, once pre-RT
    void set_tempo(double bpm) noexcept;           // alloc-free, RT-safe

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

    /// Recompute EQ coefficients, drive normalization and the delay line
    /// length from p_ / sr_ / bpm_. Pure math — RT-callable.
    void recalc_() noexcept;

    /// One transposed-direct-form-II biquad sample (defined in fx.cpp).
    static float tick_(Coefs const& c, State& s, float x) noexcept;

    FxParams p_{};
    double sr_{48000.0};
    double bpm_{120.0};

    Coefs low_{}, mid_{}, high_{};
    State low_s_[2]{}, mid_s_[2]{}, high_s_[2]{};  // per channel [L, R]
    float drive_{1.f};        // clamped drive (> bypass threshold == active)
    float drive_norm_{1.f};   // 1 / tanh(drive_) (soft shape only)

    // Compressor: per-channel envelope follower (asymmetric smoothing).
    float comp_env_[2]{0.f, 0.f};

    // Bitcrusher: held quantized output + downsample phase counter.
    float crush_hold_[2]{0.f, 0.f};
    std::uint32_t crush_phase_{0};

    std::vector<float> delay_l_, delay_r_;  // fixed 2 s max ring, pre-allocated
    std::uint32_t delay_cap_{0};
    std::uint32_t delay_len_{0};            // current musical length (samples)
    std::uint32_t delay_w_{0};

    std::vector<float> cho_l_, cho_r_;      // fixed 20 ms max ring, pre-allocated
    std::uint32_t cho_cap_{0};
    std::uint32_t cho_w_{0};
    double cho_phase_{0.0};

    // Flanger: fixed 5 ms max ring per channel + LFO phase.
    std::vector<float> fla_l_, fla_r_;
    std::uint32_t fla_cap_{0};
    std::uint32_t fla_w_{0};
    double fla_phase_{0.0};

    // Tremolo: free-running stereo LFO phase.
    double trem_phase_{0.0};

    // Reverb (insert block at chain end).
    Reverb reverb_{};
};

}  // namespace ghost_story::audio
