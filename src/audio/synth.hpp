// addu94 — src/audio/synth.hpp
//
// Subtractive synth (spec §4.2): 2 oscillators + unison, ADSR, SVF filter
// (12/24 dB, LP/HP/BP), LFO routable to filter/pitch/amp. ONE header-defined
// contract; the DSP lives in synth.cpp. Voice pool is pre-allocated — the
// audio callback NEVER allocates. Voice limit: 64.
//
// RT contract (prime directive): render()/note_on()/note_off()/
// set_param() are called from the audio thread or with the audio thread
// stopped. No locks, no allocation, no logging inside. Parameter changes
// from the UI go through SPSC queues handled by the engine, not here.
#pragma once

#include <array>
#include <cstdint>

namespace addu94::audio {

inline constexpr int kMaxVoices = 64;

enum class Waveform : std::uint8_t { Saw, Square, Triangle, Sine, Pulse25 };
enum class FilterMode : std::uint8_t { LP12, LP24, HP12, BP12 };
enum class LfoTarget : std::uint8_t { Off, Filter, Pitch, Amp };

struct SynthParams {
    // Oscillators
    Waveform osc1{Waveform::Saw};
    Waveform osc2{Waveform::Saw};
    float osc_mix{0.5f};        // 0 = osc1, 1 = osc2
    int osc2_semitones{0};      // -24..+24
    float osc2_detune_cents{0.f};  // ±100
    int unison_voices{1};       // 1..3 per osc (detuned stack)
    float unison_spread_cents{10.f};

    // Amp envelope (seconds)
    float attack{0.005f}, decay{0.15f}, sustain{0.7f}, release{0.25f};

    // Filter (SVF)
    FilterMode filter_mode{FilterMode::LP24};
    float cutoff_base_hz{1200.f};   // 20..18000
    float cutoff_keytrack{0.3f};    // 0..1 (follows note)
    float resonance{0.7f};          // 0.5..~10 (Q)

    // LFO
    LfoTarget lfo_target{LfoTarget::Off};
    float lfo_rate_hz{5.0f};        // 0.05..20
    float lfo_depth{0.2f};          // amount
};

/// One voice — plain struct, no methods with side effects.
struct Voice {
    bool active{false};
    std::uint8_t note{0};
    std::uint8_t channel{0};
    float velocity{1.f};
    double phase1{0.0}, phase2{0.0};
    float env{0.f};            // amp envelope state 0..1
    int env_stage{0};          // 0=attack 1=decay 2=sustain 3=release
    float filter_state1{0.f}, filter_state2{0.f};  // SVF states
    float lfo_phase{0.f};
    float last_amp{0.f};       // for click-free release
};

/// The synthesizer: owns a pre-allocated voice pool.
class Synth {
public:
    Synth() noexcept;

    void set_sample_rate(double sr) noexcept;
    void set_params(SynthParams const& p) noexcept;  // trivially copyable

    /// RT: start a voice. Reuses the oldest quietest voice when full.
    void note_on(int channel, int note, float velocity) noexcept;
    /// RT: release matching voice(s).
    void note_off(int channel, int note) noexcept;
    /// RT: stop all sound (panic / track mute).
    void all_sound_off() noexcept;

    /// RT: render stereo planar. No allocation, no locks.
    void render(float* left, float* right, std::uint32_t frames) noexcept;

    [[nodiscard]] int active_voices() const noexcept;

private:
    SynthParams p_{};
    double sr_{48000.0};
    std::array<Voice, kMaxVoices> voices_{};
    int oldest_hint_{0};
};

}  // namespace addu94::audio
