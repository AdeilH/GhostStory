// ghost_story — src/audio/synth.hpp
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

namespace ghost_story::audio {

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
    float velocity_filter{0.f};     // 0..1: velocity opens the cutoff
                                    // (±velocity_filter octaves around the
                                    // 0.5-velocity unity point)

    // Mono / glide (bass & lead essentials). In mono_legato mode a channel
    // owns ONE voice: a new note while a voice sounds retargets it (envelope
    // keeps running — legato) and, with glide_seconds > 0, glides the pitch
    // exponentially from the running pitch to the new note over that time.
    // Glide is only active in mono mode (predictable poly behavior).
    bool mono_legato{false};
    float glide_seconds{0.f};       // 0 = off, 0.001..2

    // LFO (routable: filter cutoff / pitch vibrato / amp tremolo)
    LfoTarget lfo_target{LfoTarget::Off};
    float lfo_rate_hz{5.0f};        // 0.05..20
    float lfo_depth{0.2f};          // amount

    // Second LFO (same targets; both apply when routed to the same
    // destination — filter factors multiply, pitch multiplies, amp stacks).
    LfoTarget lfo2_target{LfoTarget::Off};
    float lfo2_rate_hz{5.0f};       // 0.05..20
    float lfo2_depth{0.2f};         // amount
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
    float lfo2_phase{0.f};     // second LFO phase
    bool glide_on{false};      // pitch ramp active (mono mode)
    float glide_hz{0.f};       // running pitch while gliding
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
    // Per-voice state that does not fit into the fixed Voice struct (unison
    // phase copies, right-channel SVF states, LP24 section-2 states). This
    // USED to be a process-global side table (g_ext) — that was safe only
    // while exactly one Synth existed. The 4-track play engine runs three
    // Synths concurrently, and two instances writing the same global voice
    // slots would clobber each other's filter/unison state, so the table
    // moved into the object. Still fixed-size and never resized: the RT
    // contract (no allocation on note_on/render) is unchanged.
    struct VoiceExt {
        double phase1b[2]{0.0, 0.0};  // unison copies 1,2 — osc1
        double phase2b[2]{0.0, 0.0};  // unison copies 1,2 — osc2
        float lp1r{0.f};              // right-channel SVF section 1, low state
        float bp1r{0.f};              // right-channel SVF section 1, band state
        float lp2[2]{0.f, 0.f};       // LP24 section 2, low state  [L, R]
        float bp2[2]{0.f, 0.f};       // LP24 section 2, band state  [L, R]
    };

    SynthParams p_{};
    double sr_{48000.0};
    std::array<Voice, kMaxVoices> voices_{};
    std::array<VoiceExt, kMaxVoices> ext_{};
    int oldest_hint_{0};
};

}  // namespace ghost_story::audio
