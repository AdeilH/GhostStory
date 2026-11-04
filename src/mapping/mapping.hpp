// addu94 — src/mapping/mapping.hpp
//
// MappingEngine (spec §3/§5): consumes decoded MidiEvents and produces
// commands + parameter changes according to the active mode and profile.
// Manifest-driven, profile-remappable, modes PLAY/EDIT/STEP, hardware
// chords (undo/redo via simultaneous pads), zero dead controls (the M4
// audit asserts every manifest control has a binding in some mode).
#pragma once

#include "mapping/actions.hpp"
#include "midi/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace addu94::mapping {

enum class Mode : std::uint8_t { Play, Edit, Step };

/// A resolved command for the app/UI layer (queued, non-RT side effects).
struct Command {
    Action action{Action::None};
    int param{0};        // action-specific: value 0..127, track idx, bars...
    int delta{0};        // for knob-style relative actions (already scaled)
};

/// Engine-side parameter changes (e.g., synth cutoff) — applied via the
/// audio engine's SPSC queue by the caller, not directly from here.
class MappingEngine {
public:
    MappingEngine();

    /// Consume one decoded event; returns commands to execute.
    /// Runs on the app thread (mapping is NOT on the RT path).
    [[nodiscard]] std::vector<Command> consume(midi::MidiEvent const& ev);

    /// Profile text (TOML) — parsed on set; false + error on malformed.
    bool load_profile(std::string const& toml_text, std::string& error);
    [[nodiscard]] std::string const& profile_error() const { return profile_error_; }

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    void set_mode(Mode m) noexcept;
    void toggle_pad_bank() noexcept { pad_bank_b_ = !pad_bank_b_; }
    [[nodiscard]] bool pad_bank_b() const noexcept { return pad_bank_b_; }

    /// Controller mirror state (spec §5): knob values 0..127 per index.
    [[nodiscard]] std::uint8_t knob_value(int idx) const noexcept {
        return idx >= 0 && idx < 8 ? knob_values_[static_cast<std::size_t>(idx)] : 0;
    }

    /// Resolved CC number for knob `idx` (0-based): the profile's
    /// `knobs.N.cc` when set, else the first stock set position (CC 1..8).
    /// For UI display (Device view knob table). Out of range -> 0.
    [[nodiscard]] std::uint8_t knob_cc(int idx) const noexcept {
        return idx >= 0 && idx < 8 ? knob_cc_[static_cast<std::size_t>(idx)]
                                   : std::uint8_t{0};
    }

private:
    void load_default_profile();
    [[nodiscard]] std::vector<Command> map_cc(std::uint8_t cc, std::uint8_t val);
    [[nodiscard]] std::vector<Command> map_note(int note, bool on);

    /// Knob index for a CC that matches no explicit profile entry: stock
    /// MPK programs ship several fixed knob CC layouts (mk2/mk3 commonly
    /// 21..28 or 70..77; the old default only knew 1..8 — user report:
    /// "the knobs are doing nothing at all"). Each set maps knobs 1..8 by
    /// POSITION in the set. Returns -1 for a CC in none of them.
    [[nodiscard]] static int knob_alias_index(std::uint8_t cc) noexcept;

    Mode mode_{Mode::Play};
    bool pad_bank_b_{false};
    std::uint8_t knob_cc_[8]{1, 2, 3, 4, 5, 6, 7, 8};  // from profile
    std::uint8_t knob_values_[8]{};

    std::string profile_error_;
};

/// The shipped default profile (parsed via toml_min — same path as user
/// profiles). Returned as TOML text.
[[nodiscard]] std::string const& default_profile_toml();

}  // namespace addu94::mapping
