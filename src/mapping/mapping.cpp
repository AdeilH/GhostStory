// addu94 — src/mapping/mapping.cpp
#include "mapping/mapping.hpp"

#include "core/toml_min.hpp"
#include "log/logger.hpp"

#include <algorithm>

namespace addu94::mapping {

// Default profile (spec §5 mode layout). Knob/pad numbers are 1-based here
// for user readability; internals are 0-based.
std::string const& default_profile_toml() {
    static std::string const kDefault = R"(# addu94 default profile — all bindings remappable per project.
# knob.N.cc: CC number of hardware knob N (verified live on MPK mini 3:
# the stock program does NOT use CC70-77; calibrate with `addu94 probe`).

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
# Bank A (notes 36..43): drum kit / chord slots; Bank B (44..51): clip launch.
a.note = 36
b.note = 44

[modes]
# EDIT mode entry: hold Bank-A pad 8 while pressing Bank-B pad 8 (chord).
# Undo/redo chords: Bank-A pad 1 + pad 2 (undo), pad 1 + pad 3 (redo).
undo.pads = "36,37"
redo.pads = "36,38"
edit.chord_pads = "43,51"
step.chord_pads = "42,50"
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
    if (mode_ != m) {
        mode_ = m;
        pad_bank_b_ = false;
    }
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

std::vector<Command> MappingEngine::map_cc(std::uint8_t cc, std::uint8_t val) {
    std::vector<Command> out;
    // Find knob index by CC.
    for (int i = 0; i < 8; ++i) {
        if (knob_cc_[static_cast<std::size_t>(i)] != cc) continue;
        knob_values_[static_cast<std::size_t>(i)] = val;
        Command c;
        c.param = val;
        if (mode_ == Mode::Play) {
            static Action const play_actions[8] = {
                Action::TrackVolume,  Action::TrackPan,    Action::TrackSend,
                Action::InstrumentMacro1, Action::FilterCutoff,
                Action::FilterReso,   Action::EnvAttack,   Action::EnvRelease};
            c.action = play_actions[i];
        } else if (mode_ == Mode::Edit) {
            static Action const edit_actions[8] = {
                Action::ScrubCoarse, Action::ScrubFine, Action::TempoNudge,
                Action::LoopBarsSet, Action::QuantizeGridSet,
                Action::MasterVolume, Action::ClickLevel, Action::None};
            c.action = edit_actions[i];
        } else {  // Step
            // Knob 1 = selected step velocity; others no-op in M3.
            c.action = i == 0 ? Action::None : Action::None;
        }
        out.push_back(c);
        break;
    }
    return out;
}

std::vector<Command> MappingEngine::map_note(int note, bool on) {
    std::vector<Command> out;
    // Keys (channel 0/1, notes outside pad ranges) go straight to the
    // instrument path in M3; mapping only handles pads (36..51).
    bool const is_pad = note >= 36 && note <= 51;
    if (!is_pad) return out;

    // Pad actions fire on RELEASE so two-pad chords can form. A second pad
    // pressed while one is held becomes a chord; the held pad's single
    // action is suppressed in favour of the chord action.
    if (on) {
        if (pending_pad_ == 255) {
            pending_pad_ = static_cast<std::uint8_t>(note);
            return out;
        }
        if (note == pending_pad_) return out;  // retrigger while held
        // Chord: order-independent pair check.
        int const a = std::min<int>(pending_pad_, note);
        int const b = std::max<int>(pending_pad_, note);
        pending_pad_ = 255;
        if (a == 36 && b == 37) out.push_back({Action::Undo, 0, 0});
        if (a == 36 && b == 38) out.push_back({Action::Redo, 0, 0});
        if (a == 43 && b == 51) out.push_back({Action::ModeEdit, 0, 0});
        if (a == 42 && b == 50) out.push_back({Action::ModeStep, 0, 0});
        return out;
    }

    // Pad release.
    if (pending_pad_ != 255) {
        if (note == pending_pad_) {
            pending_pad_ = 255;
            int const pad = note - (pad_bank_b_ ? 44 : 36);
            if (pad >= 0 && pad < 8) {
                Command c;
                if (mode_ == Mode::Play) {
                    c.action = pad_bank_b_ ? Action::ClipLaunch
                                           : Action::ChordSlot;
                } else if (mode_ == Mode::Edit) {
                    c.action = Action::TrackSelect;
                } else {
                    c.action = Action::None;  // step grid: pattern editor
                }
                c.param = pad;
                out.push_back(c);
            }
        }
    }
    return out;
}

}  // namespace addu94::mapping
