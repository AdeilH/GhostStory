// ghost_story — src/audio/sampler.hpp
//
// 64-voice sample player (spec §4.1/§4.3): WAV zones are preloaded on the
// control thread; playback is one-shot with linear interpolation, a
// velocity^1.5 gain curve, per-pad tuning (resample), per-pad exponential
// decay scaling and choke groups for drum-kit mode.
//
// RT contract (mirrors synth.hpp): note_on()/note_off()/all_sound_off()/
// render() never allocate, lock, log or make syscalls. All state lives in
// the Sampler object itself (voice pool + zone table) — deliberately NO
// file-local side tables, so multiple Samplers coexist safely.
//
// Sample memory strategy: zones are held as shared_ptr<const SampleData> in
// the zone table for the object's lifetime; voices hold copies (an atomic
// refcount bump in note_on — no lock). add_zone/add_zone_data park replaced
// zones in a control-thread retirement list instead of freeing them, so
// sample memory is NEVER deallocated from the audio thread.
//
// Pad mapping (drum-kit mode, spec §4.3): notes 36..43 are pads 0..7 (Bank
// A); notes 44..51 (Bank B) address the same pads with an extra +12
// semitone shift — PLAY mode routes both banks here, so all 16 hardware
// pads drum. An exact zone loaded at the literal note (add_zone/
// load_override accept 36..51) is played as-is before the pad mapping
// applies. Notes outside 36..51 are natural one-shots (no tuning/decay/
// choke).
//
// Output: per-frame soft clip (tanh) after voice summation keeps the mix in
// (-1, 1) under heavy polyphony (64 stacked one-shots would otherwise sum
// far past full scale). At normal levels tanh is transparent; it is exact 0
// for a silent mix, preserving the "no voices => exact zeros" guarantee.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/wav_file.hpp"

namespace ghost_story::audio {

inline constexpr int kSamplerMaxVoices = 64;
inline constexpr int kPadBaseNote = 36;  // Bank A pad 1 (kick)
inline constexpr int kPadCount = 8;

struct SamplerParams {
    float gain{1.0f};
    // per-pad/zone tuning + decay (drum-kit mode, spec §4.3):
    std::array<int, 8> pad_tuning_semitones{};   // -24..+24 per pad
    std::array<float, 8> pad_decay_scale{};      // 0.1..4.0, 1 = natural
                                                 // (0/unset = natural too)
    std::array<int, 8> pad_choke_group{};        // 0 = none; same group chokes
};

class Sampler {
public:
    Sampler() noexcept;

    /// Control thread. Load a WAV file as the zone for `midi_note`
    /// (replaces any existing zone; the old one is retired, never freed
    /// on the audio thread). Returns false + `error` on failure.
    bool add_zone(int midi_note, std::string const& wav_path,
                  std::string& error);
    /// Control thread. Same, from in-memory data (DrumKit fallbacks) —
    /// keeps the zero-external-files path free of temp files.
    bool add_zone_data(int midi_note, WavData const& data, std::string& error);

    void set_params(SamplerParams const& p) noexcept;  // trivially copyable
    void set_sample_rate(double) noexcept;

    /// RT: start a one-shot voice. Reuses the oldest quietest voice when
    /// full. A nonzero choke group fades same-group voices out over 2 ms.
    void note_on(int channel, int note, float velocity) noexcept;
    /// RT: one-shot semantics — samples play to their natural end, so this
    /// is a no-op (choke groups trigger on note_on, not note_off).
    void note_off(int channel, int note) noexcept;
    /// RT: instant silence (panic). Next render is exact zeros.
    void all_sound_off() noexcept;

    /// RT: render stereo planar with overwrite semantics (zeroes the
    /// buffers first, like Synth). No allocation, no locks.
    void render(float* left, float* right, std::uint32_t frames) noexcept;

    [[nodiscard]] int active_voices() const noexcept;
    [[nodiscard]] bool has_zone(int midi_note) const noexcept;

private:
    struct SampleData {
        std::uint32_t sample_rate{48000};
        std::uint16_t channels{1};
        std::uint32_t frames{0};
        std::vector<float> samples;  // interleaved, read-only on RT path
    };

    struct Voice {
        bool active{false};
        std::uint8_t note{0};
        std::uint8_t channel{0};
        std::uint8_t choke_group{0};
        float gain{0.f};          // velocity^1.5 * params.gain (frozen)
        float rate{1.f};          // source frames per output frame
        float decay_factor{1.f};  // per-sample exponential decay
        float env{1.f};           // decay envelope
        float fade{1.f};          // choke fade-out 1..0 (linear)
        float fade_step{0.f};     // per-sample fade decrement
        double pos{0.0};          // source frame position (double: >2^24
                                  // frames lose float precision)
        std::shared_ptr<SampleData const> sample;
    };

    SamplerParams p_{};
    double sr_{48000.0};
    std::array<Voice, kSamplerMaxVoices> voices_{};
    std::array<std::shared_ptr<SampleData const>, 128> zones_{};
    std::vector<std::shared_ptr<SampleData const>> retired_;  // control thread
    int oldest_hint_{0};
};

}  // namespace ghost_story::audio
