// ghost_story — src/mapping/mapping.cpp
#include "mapping/mapping.hpp"

#include "core/toml_min.hpp"
#include "log/logger.hpp"

#include <algorithm>

namespace ghost_story::mapping {

// Default profile (spec §5 mode layout). Knob/pad numbers are 1-based here
// for user readability; internals are 0-based.
std::string const& default_profile_toml() {
    static std::string const kDefault = R"(# ghost_story default profile — all bindings remappable per project.
# knob.N.cc: CC number of hardware knob N. The defaults below are knob
# positions 1..8 of the FIRST stock MPK knob set (CC 1..8). The mapping
# engine ALSO recognizes the other stock sets by position: CC 21..28 and
# CC 70..77 (mk2/mk3 factory programs ship these). A `knobs.N.cc` entry
# here OVERRIDES the alias sets for that knob — run `ghost_story calibrate` to
# generate one from your actual hardware.

[knobs]
1.cc = 1    # track volume
2.cc = 2    # track pan
3.cc = 3    # track send
4.cc = 4    # instrument macro 1
5.cc = 5    # filter cutoff
6.cc = 6    # filter reso
7.cc = 7    # env attack
8.cc = 8    # env release

[pads]
# PLAY: all 16 pads fire the drum kit — 36..43 are the 8 kit sounds,
# 44..51 the same sounds one octave up. No utility pads in PLAY, so
# drumming can never fire Undo.
# EDIT: 36..43 = track select, 44/45 = undo/redo, 46..51 = chord memory
# slots (tap = play chord, hold + keys = capture).
# KIT (the Step mode): every pad selects one of the 16 built-in packs
# and auditions its voice. No two-pad chords: drumming must never
# accidentally erase a take.
a.note = 36
b.note = 44
)";
    return kDefault;
}

MappingEngine::MappingEngine() { load_default_profile(); }

void MappingEngine::load_default_profile() {
    std::string err;
    if (!load_profile(default_profile_toml(), err)) {
        // Never happens (shipped text is tested) — log loudly if it does.
        log::critical("mapping", "default profile failed to parse: {}", err);
    }
}

bool MappingEngine::load_profile(std::string const& toml_text,
                                 std::string& error) {
    core::TomlError te;
    auto table = core::toml_parse(toml_text, te);
    if (!table) {
        error = "line " + std::to_string(te.line) + ": " + te.message;
        profile_error_ = error;
        return false;
    }
    profile_error_.clear();
    error.clear();
    // M3: knobs 1-8 from [knobs] N.cc; bindings table lands with the
    // full M4 binding system (this parse validates + stores knobs now).
    for (int k = 1; k <= 8; ++k) {
        auto const key = "knobs." + std::to_string(k) + ".cc";
        auto const it = table->find(key);
        if (it != table->end()) {
            knob_cc_[static_cast<std::size_t>(k - 1)] =
                static_cast<std::uint8_t>(it->second.as_int(k));
        }
    }
    return true;
}

void MappingEngine::set_mode(Mode m) noexcept {
    mode_ = m;
}

std::vector<Command> MappingEngine::consume(midi::MidiEvent const& ev) {
    std::vector<Command> out;
    switch (ev.type) {
    case midi::Type::CC:
        if (ev.d1 < 120) {  // 120+ = channel mode messages, ignore
            knob_values_[ev.d1 % 8] = ev.d2;  // provisional mirror slot
            out = map_cc(ev.d1, ev.d2);
        }
        break;
    case midi::Type::NoteOn:
    case midi::Type::NoteOff:
        out = map_note(ev.note(), ev.is_note_on());
        break;
    default:
        break;
    }
    return out;
}

int MappingEngine::knob_alias_index(std::uint8_t cc) noexcept {
    // The three stock MPK knob CC sets, matched by position-in-set. Kept as
    // explicit ranges (not a table scan) so the intent reads at a glance.
    if (cc >= 1 && cc <= 8) return cc - 1;
    if (cc >= 21 && cc <= 28) return cc - 21;
    if (cc >= 70 && cc <= 77) return cc - 70;
    return -1;
}

// Per-mode knob bindings, in knob order 1..8. ONE table pair serves both
// consumers: map_cc (hardware CCs) and ControllerMirror (interactive
// on-screen knobs) — plus knob_label for the text under each arc, which
// previously lived in the UI and had to be kept in sync by hand.
namespace {

Action const kPlayKnobActions[8] = {
    Action::TrackVolume,      Action::TrackPan,     Action::TrackSend,
    Action::InstrumentMacro1, Action::FilterCutoff, Action::FilterReso,
    Action::EnvAttack,        Action::EnvRelease};
char const* const kPlayKnobLabels[8] = {
    "vol", "pan", "send", "macro", "cut", "res", "atk", "rel"};

Action const kEditKnobActions[8] = {
    Action::FxSelect,    Action::FxToggle,         Action::FxParamA,
    Action::FxParamB,    Action::TempoNudge,       Action::LoopBarsSet,
    Action::QuantizeGridSet, Action::MasterVolume};
char const* const kEditKnobLabels[8] = {
    "fx.sel", "fx.on", "fx.A", "fx.B", "tempo", "loop", "quant", "master"};

// KIT = the sound-design page. Knobs 5..7 are deliberate no-ops for now.
Action const kKitKnobActions[8] = {
    Action::VelocityCurveSet, Action::KitGain, Action::PadTune,
    Action::PadDecay,         Action::None,    Action::None,
    Action::None,             Action::MasterVolume};
char const* const kKitKnobLabels[8] = {
    "curve", "gain", "tune", "decay", "-", "-", "-", "master"};

}  // namespace

Action MappingEngine::knob_action(Mode mode, int idx) noexcept {
    if (idx < 0 || idx >= 8) return Action::None;
    switch (mode) {
    case Mode::Play: return kPlayKnobActions[static_cast<std::size_t>(idx)];
    case Mode::Edit: return kEditKnobActions[static_cast<std::size_t>(idx)];
    case Mode::Step: return kKitKnobActions[static_cast<std::size_t>(idx)];
    }
    return Action::None;
}

char const* MappingEngine::knob_label(Mode mode, int idx) noexcept {
    if (idx < 0 || idx >= 8) return nullptr;
    switch (mode) {
    case Mode::Play: return kPlayKnobLabels[static_cast<std::size_t>(idx)];
    case Mode::Edit: return kEditKnobLabels[static_cast<std::size_t>(idx)];
    case Mode::Step: return kKitKnobLabels[static_cast<std::size_t>(idx)];
    }
    return nullptr;
}

std::vector<Command> MappingEngine::map_cc(std::uint8_t cc, std::uint8_t val) {
    std::vector<Command> out;
    // Find knob index by CC: an explicit profile entry wins, then the stock
    // alias sets (profile override wins per the contract — a calibrated
    // knob keeps its CC even if it collides with an alias position).
    int idx = -1;
    for (int i = 0; i < 8; ++i) {
        if (knob_cc_[static_cast<std::size_t>(i)] == cc) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = knob_alias_index(cc);
    if (idx >= 0) {
        knob_values_[static_cast<std::size_t>(idx)] = val;
        Command c;
        c.param = val;
        // Play = the mix page (volume/pan/send + the selected track's
        // instrument). Edit = the FX page: knobs 1-4 drive the selected FX
        // slot (select / toggle / param A / param B — see PlayEngine
        // fx_slot_param_name for the per-slot meanings), knobs 5-8 keep the
        // transport controls. KIT = the sound-design page (velocity curve,
        // kit gain, last-pad tune/decay, master). The old scrub knobs were
        // stored-only (no transport position control exists) and click
        // level lives on the transport-bar slider.
        c.action = knob_action(mode_, idx);
        out.push_back(c);
        return out;
    }
    // Unmapped CC (user report: "the knobs are doing nothing at all" — the
    // user deserves to learn WHY, once per CC number per process so a
    // sweeping knob spams at most one line). Sustain (64) and channel-mode
    // messages (>= 120, already filtered in consume()) are never knobs.
    static bool logged[128] = {};
    if (cc != 64 && !logged[cc]) {
        logged[cc] = true;
        log::info("mapping",
                  "unmapped CC {}={} — run `ghost_story calibrate` or add "
                  "knobs.N.cc to profile.toml",
                  cc, val);
    }
    return out;
}

std::vector<Command> MappingEngine::map_note(int note, bool on) {
    // Pad semantics v2 (user report, round 2: "BANK B doesn't work at all"
    // + "no other pack on pad working"): v0.1.3 gave bank B to utilities
    // and chord slots, which left a drummer's bank B SILENT — the pads'
    // primary job in PLAY is making sound. Per mode:
    //
    // PLAY  — a pure drum machine: ALL 16 pads fire the kit. 36..43
    //         natural, 44..51 the same sounds +12 (Sampler pad mapping).
    //         No utility pads here, so drumming can never fire Undo —
    //         stronger than the v0.1.3 "no accidental undo" guarantee.
    //         Param = absolute pad index 0..15 (mirror pulse).
    // EDIT  — bank A selects the track the keys play (press); bank B:
    //         44/45 undo/redo, 46..51 chord slots 0..5 (press/release
    //         pairs; hold + keys = capture). Pads don't drum in EDIT —
    //         chord capture stays quiet.
    // KIT (Step) — the pack browser: every pad 36..51 selects that pack
    //         (0..15) and PlayEngine auditions its voice, so "no other
    //         pack on pad" can't happen: the kit is ON the pads.
    std::vector<Command> out;
    if (note < 36 || note > 51) return out;

    auto push = [&](Action a, int param, int delta) {
        out.push_back(Command{a, param, delta});
    };

    if (mode_ == Mode::Play) {
        if (on) push(Action::DrumPad, note - 36, 0);
        return out;
    }

    if (note <= 43) {  // bank A: track select (EDIT) / pack select (KIT)
        if (mode_ == Mode::Edit) {
            if (on) push(Action::TrackSelect, note - 36, 0);
        } else {  // KIT
            if (on) push(Action::PadPackSelect, note - 36, 0);
        }
        return out;
    }

    // Bank B 44..51.
    if (mode_ == Mode::Edit) {
        if (note == 44 || note == 45) {
            if (on) push(note == 44 ? Action::Undo : Action::Redo, 0, 0);
        } else {
            // Chord slots 0..5: press/release pairs so the engine can
            // sound the chord on press and stop it on release.
            push(Action::ChordSlot, note - 46, on ? 0 : 1);
        }
    } else {  // KIT: packs 8..15 on bank B
        if (on) push(Action::PadPackSelect, note - 36, 0);
    }
    return out;
}

}  // namespace ghost_story::mapping
