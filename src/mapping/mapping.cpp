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
# knob.N.cc: CC number of hardware knob N. The defaults below are knob
# positions 1..8 of the FIRST stock MPK knob set (CC 1..8). The mapping
# engine ALSO recognizes the other stock sets by position: CC 21..28 and
# CC 70..77 (mk2/mk3 factory programs ship these). A `knobs.N.cc` entry
# here OVERRIDES the alias sets for that knob — run `addu94 calibrate` to
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
# PLAY: 36..43 = drum pads (8 distinct kit sounds), 44/45 = undo/redo,
# 46..51 = chord memory slots (tap = play chord, hold + keys = capture).
# EDIT: 36..43 = track select. No two-pad chords: drumming must never
# accidentally erase a take (the old A1+A2 Undo chord did exactly that).
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

int MappingEngine::knob_alias_index(std::uint8_t cc) noexcept {
    // The three stock MPK knob CC sets, matched by position-in-set. Kept as
    // explicit ranges (not a table scan) so the intent reads at a glance.
    if (cc >= 1 && cc <= 8) return cc - 1;
    if (cc >= 21 && cc <= 28) return cc - 21;
    if (cc >= 70 && cc <= 77) return cc - 70;
    return -1;
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
        if (mode_ == Mode::Play) {
            static Action const play_actions[8] = {
                Action::TrackVolume,  Action::TrackPan,    Action::TrackSend,
                Action::InstrumentMacro1, Action::FilterCutoff,
                Action::FilterReso,   Action::EnvAttack,   Action::EnvRelease};
            c.action = play_actions[idx];
        } else if (mode_ == Mode::Edit) {
            static Action const edit_actions[8] = {
                Action::ScrubCoarse, Action::ScrubFine, Action::TempoNudge,
                Action::LoopBarsSet, Action::QuantizeGridSet,
                Action::MasterVolume, Action::ClickLevel, Action::None};
            c.action = edit_actions[idx];
        } else {  // Step
            // Knob 1 = selected step velocity; others no-op in M3.
            c.action = idx == 0 ? Action::None : Action::None;
        }
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
                  "unmapped CC {}={} — run `addu94 calibrate` or add "
                  "knobs.N.cc to profile.toml",
                  cc, val);
    }
    return out;
}

std::vector<Command> MappingEngine::map_note(int note, bool on) {
    // FIX (user report: "pad 1 and pad 2 have same response"): the old
    // scheme made A1+A2 the Undo chord — drumming A1-then-A2 erased takes,
    // and ChordSlot/ClipLaunch were never implemented so pads had no
    // distinct responses. New PLAY-mode semantics, all immediate and
    // distinct (bank determined by note range, no bank toggle needed):
    //   36..43  drum pads      -> DrumPad (sound via RT path, distinct hit)
    //   44      Undo           (press, instant)
    //   45      Redo           (press, instant)
    //   46..51  chord slots 0..5 (press/release pairs; hold + keys = capture)
    // EDIT mode: 36..43 selects tracks (press); 44/45 undo/redo still work.
    std::vector<Command> out;
    if (note < 36 || note > 51) return out;

    auto push = [&](Action a, int param, int delta) {
        out.push_back(Command{a, param, delta});
    };

    if (note <= 43) {  // bank A: drum pads / track select
        int const pad = note - 36;
        if (mode_ == Mode::Edit) {
            if (on) push(Action::TrackSelect, pad, 0);
        } else if (mode_ == Mode::Step) {
            // step grid: pattern editor (M-later); mirror pulse only
            if (on) push(Action::None, pad, 0);
        } else {
            if (on) push(Action::DrumPad, pad, 0);
        }
        return out;
    }

    // Bank B: utilities + chord memory.
    if (note == 44 || note == 45) {
        if (on) push(note == 44 ? Action::Undo : Action::Redo, 0, 0);
        return out;
    }
    int const slot = note - 46;  // 0..5
    if (mode_ == Mode::Play) {
        push(Action::ChordSlot, slot, on ? 0 : 1);
    }
    return out;
}

}  // namespace addu94::mapping
