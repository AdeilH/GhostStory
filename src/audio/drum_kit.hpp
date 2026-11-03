// addu94 — src/audio/drum_kit.hpp
//
// Drum-kit mode glue (spec §4.1/§4.3): pads on channel 9; notes 36..43 are
// Bank A pads 1..8, notes 44..51 the same pads +12 (handled inside the
// Sampler's pad mapping).
//
// Zero-external-files playability: init() synthesizes 8 deterministic
// fallback one-shots (kick, rim, snare, clap, tom, closed hat, open hat,
// crash) with simple DSP — sine sweeps, noise bursts (seeded LCG, never
// rand()), exponential envelopes — rendered ONCE on the control thread and
// injected into the Sampler as in-memory zones. load_override() replaces
// individual pads with WAV files. All RT work is delegated to the Sampler.
#pragma once

#include <cstdint>
#include <string>

#include "audio/sampler.hpp"

namespace addu94::audio {

class DrumKit {
public:
    /// Control thread. Synthesizes the fallback kit at `sample_rate` into
    /// memory. Returns false + `error` on failure (bad rate, OOM).
    bool init(double sample_rate, std::string& error);

    /// Control thread. Replace one pad's zone with a WAV file (notes
    /// 36..51 address the pads; other notes are passed through to the
    /// Sampler's zone table unchanged).
    bool load_override(int midi_note, std::string const& wav_path,
                       std::string& error);

    void set_params(SamplerParams const& p) noexcept;

    /// Control thread. Re-synthesizes the fallback kit at the new rate
    /// (fallbacks are rendered once per rate). noexcept: a synthesis
    /// failure (OOM) downgrades to "no fallback kit" rather than throwing.
    void set_sample_rate(double sample_rate) noexcept;

    /// RT: pads. 36..43 = Bank A, 44..51 = same pads +12.
    void note_on(int note, float velocity) noexcept;

    /// RT: overwrite semantics, delegated to the Sampler.
    void render(float* left, float* right, std::uint32_t frames) noexcept;

    /// Direct access (all_sound_off, has_zone, choke/tuning params...).
    [[nodiscard]] Sampler& sampler() noexcept;

private:
    bool synthesize_fallbacks(std::string& error);

    Sampler sampler_{};
    double sr_{48000.0};
    bool initialized_{false};
};

}  // namespace addu94::audio
