// ghost_story — src/mapping/actions.hpp
//
// Actions: what a physical control DOES (spec §5). Bindings live in
// per-project profile.toml; defaults are embedded (see mapping.hpp) and
// parsed through the same TOML path so user edits and shipped defaults
// go through identical code.
#pragma once

#include <cstdint>

namespace ghost_story::mapping {

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
    ClipLaunch,    // future multi-clip grid: param = clip index
    ClipStop,
    ChordSlot,     // pads bank B 3-8 (EDIT): chord memory; 0=press 1=release
    PadPackSelect, // pads in KIT (Step) mode: param = pack index 0..15

    // --- misc ---
    ArpToggle,
    ScaleLockToggle,
    KeysPlayOnceToggle,  // keys: play once per press vs allow repeat fires
    OctaveUp,
    OctaveDown,
    TempoNudge,    // knob: param = delta
    LoopBarsSet,   // knob: param = bars 1..16
    QuantizeGridSet,
    ClickLevel,
    ScrubCoarse,
    ScrubFine,

    // --- KIT mode (sound-design knob page) ---
    VelocityCurveSet,  // knob: 4 zones across the sweep -> velocity curve
    KitGain,           // knob: sampler gain 0..2 across the kit
    PadTune,           // knob: last-hit pad tuning, -12..+12 semitones
    PadDecay,          // knob: last-hit pad decay scale 0.1..4.0

    // --- FX (Edit-mode knob page; per selected track, drums included) ---
    FxSelect,      // knob: param 0..127 picks one of 9 chain slots
    FxToggle,      // press: on/off the selected slot
    FxParamA,      // knob: slot-dependent param A (see PlayEngine::fx_slot_param_name)
    FxParamB,      // knob: slot-dependent param B
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
    case Action::ClipLaunch: return "clips.launch";
    case Action::ClipStop: return "clips.stop";
    case Action::ChordSlot: return "chords.slot";
    case Action::PadPackSelect: return "pads.pack";
    case Action::ArpToggle: return "arp.toggle";
    case Action::ScaleLockToggle: return "keys.scale_lock";
    case Action::KeysPlayOnceToggle: return "keys.play_once";
    case Action::OctaveUp: return "keys.octave_up";
    case Action::OctaveDown: return "keys.octave_down";
    case Action::TempoNudge: return "transport.tempo";
    case Action::LoopBarsSet: return "transport.loop_bars";
    case Action::QuantizeGridSet: return "record.quantize";
    case Action::ClickLevel: return "transport.click_level";
    case Action::ScrubCoarse: return "transport.scrub_coarse";
    case Action::ScrubFine: return "transport.scrub_fine";
    case Action::VelocityCurveSet: return "kit.velocity_curve";
    case Action::KitGain: return "kit.gain";
    case Action::PadTune: return "kit.pad_tune";
    case Action::PadDecay: return "kit.pad_decay";
    case Action::FxSelect: return "fx.select";
    case Action::FxToggle: return "fx.toggle";
    case Action::FxParamA: return "fx.param_a";
    case Action::FxParamB: return "fx.param_b";
    }
    return "?";
}

}  // namespace ghost_story::mapping
