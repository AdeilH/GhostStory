// addu94 — src/mapping/actions.hpp
//
// Actions: what a physical control DOES (spec §5). Bindings live in
// per-project profile.toml; defaults are embedded (see mapping.hpp) and
// parsed through the same TOML path so user edits and shipped defaults
// go through identical code.
#pragma once

#include <cstdint>

namespace addu94::mapping {

enum class Action : std::uint16_t {
    None = 0,

    // --- transport (buttons/pads) ---
    TransportPlay,
    TransportStop,
    TransportRecord,
    LoopToggle,
    MetronomeToggle,
    CountInToggle,
    PunchToggle,
    Undo,
    Redo,

    // --- track/mixer (knobs) ---
    TrackVolume,
    TrackPan,
    TrackSend,
    MasterVolume,
    TrackSelect,   // pads: param = track index
    DrumPad,       // pads bank A: param = pad index (sound via RT path)

    // --- instrument (knobs, per selected track) ---
    FilterCutoff,
    FilterReso,
    EnvAttack,
    EnvRelease,
    InstrumentMacro1,

    // --- modes ---
    ModePlay,
    ModeEdit,
    ModeStep,
    PadBankToggle, // pad bank A/B switch
    ClipLaunch,    // pads bank B: param = clip index
    ClipStop,
    ChordSlot,     // pads bank B 3-8: chord memory; delta 0=press 1=release

    // --- misc ---
    ArpToggle,
    ScaleLockToggle,
    OctaveUp,
    OctaveDown,
    TempoNudge,    // knob: param = delta
    LoopBarsSet,   // knob: param = bars 1..16
    QuantizeGridSet,
    ClickLevel,
    ScrubCoarse,
    ScrubFine,
};

[[nodiscard]] constexpr const char* to_string(Action a) noexcept {
    switch (a) {
    case Action::None: return "none";
    case Action::TransportPlay: return "transport.play";
    case Action::TransportStop: return "transport.stop";
    case Action::TransportRecord: return "transport.record";
    case Action::LoopToggle: return "transport.loop";
    case Action::MetronomeToggle: return "transport.metronome";
    case Action::CountInToggle: return "transport.countin";
    case Action::PunchToggle: return "transport.punch";
    case Action::Undo: return "edit.undo";
    case Action::Redo: return "edit.redo";
    case Action::TrackVolume: return "mix.track_volume";
    case Action::TrackPan: return "mix.track_pan";
    case Action::TrackSend: return "mix.track_send";
    case Action::MasterVolume: return "mix.master";
    case Action::TrackSelect: return "track.select";
    case Action::DrumPad: return "pads.drum";
    case Action::FilterCutoff: return "synth.filter_cutoff";
    case Action::FilterReso: return "synth.filter_reso";
    case Action::EnvAttack: return "synth.env_attack";
    case Action::EnvRelease: return "synth.env_release";
    case Action::InstrumentMacro1: return "synth.macro1";
    case Action::ModePlay: return "mode.play";
    case Action::ModeEdit: return "mode.edit";
    case Action::ModeStep: return "mode.step";
    case Action::PadBankToggle: return "pads.bank";
    case Action::ClipLaunch: return "clips.launch";
    case Action::ClipStop: return "clips.stop";
    case Action::ChordSlot: return "chords.slot";
    case Action::ArpToggle: return "arp.toggle";
    case Action::ScaleLockToggle: return "keys.scale_lock";
    case Action::OctaveUp: return "keys.octave_up";
    case Action::OctaveDown: return "keys.octave_down";
    case Action::TempoNudge: return "transport.tempo";
    case Action::LoopBarsSet: return "transport.loop_bars";
    case Action::QuantizeGridSet: return "record.quantize";
    case Action::ClickLevel: return "transport.click_level";
    case Action::ScrubCoarse: return "transport.scrub_coarse";
    case Action::ScrubFine: return "transport.scrub_fine";
    }
    return "?";
}

}  // namespace addu94::mapping
