// ghost_story — src/audio/drum_kit.hpp
//
// Drum-kit mode glue (spec §4.1/§4.3): pads on channel 9; PLAY mode fires
// ALL 16 hardware pads — notes 36..43 are Bank A pads 1..8, notes 44..51
// the SAME pads one octave up (the Sampler's pad mapping applies +12).
// EDIT/KIT mode pads have their mode functions (track select, undo/redo,
// chord slots, pack select) and are not routed here — PlayEngine gates
// the drum path on the mode.
//
// Zero-external-files playability: init() synthesizes 8 deterministic
// one-shots (kick, rim, snare, clap, tom, closed hat, open hat, crash) per
// PACK with simple DSP — sine sweeps, noise bursts (seeded LCG, never
// rand()), exponential envelopes — rendered ONCE on the control thread and
// injected into per-pack Samplers as in-memory zones. load_override()
// replaces individual pads of the ACTIVE pack with WAV files. All RT work
// is delegated to the Samplers.
//
// Pad packs (user report: "can't work out how to use different packs with
// the pads"): eight built-in packs — the same 8 DSP recipes, different
// parameter tables (frequencies, decays, noise brightness). DETERMINISTIC:
// no rand(), two inits at the same rate are bit-identical.
//
// WHY prebuilt Samplers + an atomic pack index instead of re-synthesizing
// into one Sampler on load_pack(): Sampler::add_zone_data() writes the
// zone-table shared_ptr non-atomically on the control thread while the
// audio callback may be reading that same slot in note_on() — a data race.
// Synthesizing all packs up front and switching an int-sized atomic index
// keeps the RT path lock/alloc-free AND race-free; 8 packs x 8 one-shots
// is only a few MB of float samples, which is cheap. load_override() keeps
// the existing add_zone_data path (its race predates packs and is
// documented; replacing a WAV while the kit is playing is a user-initiated,
// rare control-thread action — accepted, unchanged semantics).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "audio/sampler.hpp"

namespace ghost_story::audio {

/// Single source of truth for how many built-in packs exist. pack_count()
/// and the samplers_ array (and kPackSpecs in the .cpp, sized from this)
/// all derive from it, so adding a pack touches exactly one number here
/// plus one table row there. (A stale kPacks literal once made packs 4..7
/// index out of bounds — the bug this constant exists to prevent.)
inline constexpr int kPackCount = 16;

class DrumKit {
public:
    /// Built-in pack count / names (control + app threads; pure tables).
    [[nodiscard]] static constexpr int pack_count() noexcept {
        return kPackCount;
    }
    /// nullptr for an out-of-range index; otherwise a stable static string.
    [[nodiscard]] static char const* pack_name(int idx) noexcept;

    /// Control thread. Synthesizes ALL packs at `sample_rate` into memory.
    /// Returns false + `error` on failure (bad rate, OOM). The active pack
    /// starts at 0 ("MPC Classic" — the recipes the kit shipped with).
    bool init(double sample_rate, std::string& error);

    /// Control thread. Make pack `idx` the one the RT path plays (the
    /// switching itself is a single atomic store — race-free against the
    /// audio callback). Returns false + `error` for a bad index or an
    /// uninitialized kit.
    bool load_pack(int idx, std::string& error);

    /// Currently active pack index (atomic read; app/UI display).
    [[nodiscard]] int current_pack() const noexcept {
        return active_pack_.load(std::memory_order_relaxed);
    }

    /// Control thread. Replace one pad's zone with a WAV file (notes
    /// 36..51 address the pads; other notes are passed through to the
    /// active pack's Sampler zone table unchanged).
    bool load_override(int midi_note, std::string const& wav_path,
                       std::string& error);

    /// Control thread. Same, from in-memory data — the live-mic grab path
    /// loads captured audio as a pad zone with no temp file. Same accepted
    /// zone-swap semantics as load_override (see the header note above).
    bool load_override_data(int midi_note, WavData const& data,
                            std::string& error);

    void set_params(SamplerParams const& p) noexcept;

    /// Control thread. Re-synthesizes all packs at the new rate (packs are
    /// rendered once per rate). noexcept: a synthesis failure (OOM)
    /// downgrades to "no fallback kit" rather than throwing.
    void set_sample_rate(double sample_rate) noexcept;

    /// RT: pads. 36..43 = Bank A, 44..51 = same pads +12.
    void note_on(int note, float velocity) noexcept;

    /// RT: overwrite semantics, delegated to the active pack's Sampler.
    void render(float* left, float* right, std::uint32_t frames) noexcept;

    /// Direct access (all_sound_off, has_zone, choke/tuning params...) of
    /// the ACTIVE pack's sampler.
    [[nodiscard]] Sampler& sampler() noexcept;

private:
    /// Control thread: synthesize pack `idx` into samplers_[idx].
    bool synthesize_pack(int idx, std::string& error);

    static constexpr int kPacks = kPackCount;
    std::array<Sampler, kPacks> samplers_{};
    std::atomic<int> active_pack_{0};
    double sr_{48000.0};
    bool initialized_{false};
};

}  // namespace ghost_story::audio
