// ghost_story — src/mapping/audit.cpp
#include "mapping/audit.hpp"

#include <algorithm>

namespace ghost_story::mapping {

std::vector<CoverageEntry> const& coverage_table() {
    // One entry minimum per control per the mode where it lives. Keys and
    // pedal are "always-on" instrument paths (every mode) — bound in Play.
    static std::vector<CoverageEntry> const table = [] {
        std::vector<CoverageEntry> t;
        auto add = [&](std::string const& id, Mode m, Action a) {
            t.push_back({id, m, a});
        };
        // Keys (25) — instrument input in Play.
        for (int i = 0; i < 25; ++i) {
            add("key." + std::to_string(36 + i), Mode::Play, Action::None);
        }
        // Pads: ids follow the inventory scheme — pad.<a|b><row><col> with
        // row 1..2, col 1..4 (8 pads per bank). Note numbers: bank A
        // 36..43, bank B 44..51 in the inventory's row-major order.
        // Pads semantics v2 (user report: "BANK B doesn't work at all" +
        // "no other pack on pad working"): PLAY = all 16 pads drum (B is
        // A one octave up); EDIT = bank A track select, bank B undo/redo
        // + chord slots; KIT (Step) = the 16-pack browser on all 16 pads.
        // Keep in lockstep with MappingEngine::map_note (this table has
        // drifted twice: chord slots on bank A in Play, then a Punch/
        // ClipStop row nothing emitted).
        for (int row = 1; row <= 2; ++row) {
            for (int col = 1; col <= 4; ++col) {
                std::string const id = "pad.a" + std::to_string(row) +
                                       std::to_string(col);
                add(id, Mode::Play, Action::DrumPad);
                add(id, Mode::Edit, Action::TrackSelect);
                add(id, Mode::Step, Action::PadPackSelect);
            }
        }
        static Action const play_pad_b[8] = {Action::DrumPad, Action::DrumPad,
                                             Action::DrumPad, Action::DrumPad,
                                             Action::DrumPad, Action::DrumPad,
                                             Action::DrumPad, Action::DrumPad};
        static Action const edit_pad_actions[8] = {
            Action::Undo,      Action::Redo,     Action::ChordSlot,
            Action::ChordSlot, Action::ChordSlot, Action::ChordSlot,
            Action::ChordSlot, Action::ChordSlot};
        static Action const step_pad_b[8] = {
            Action::PadPackSelect, Action::PadPackSelect, Action::PadPackSelect,
            Action::PadPackSelect, Action::PadPackSelect, Action::PadPackSelect,
            Action::PadPackSelect, Action::PadPackSelect};
        int eb = 0;
        for (int row = 1; row <= 2; ++row) {
            for (int col = 1; col <= 4; ++col) {
                std::string const id = "pad.b" + std::to_string(row) +
                                       std::to_string(col);
                add(id, Mode::Play, play_pad_b[eb]);
                add(id, Mode::Edit, edit_pad_actions[eb]);
                add(id, Mode::Step, step_pad_b[eb]);
                ++eb;
            }
        }
        // Knobs 1-8 — different roles per mode (all three covered).
        static char const* const knob_id[8] = {
            "knob.1", "knob.2", "knob.3", "knob.4",
            "knob.5", "knob.6", "knob.7", "knob.8"};
        static Action const play_k[8] = {
            Action::TrackVolume, Action::TrackPan, Action::TrackSend,
            Action::InstrumentMacro1, Action::FilterCutoff,
            Action::FilterReso, Action::EnvAttack, Action::EnvRelease};
        static Action const edit_k[8] = {
            Action::FxSelect, Action::FxToggle, Action::FxParamA,
            Action::FxParamB, Action::TempoNudge,
            Action::LoopBarsSet, Action::QuantizeGridSet,
            Action::MasterVolume};
        static Action const step_k[8] = {
            Action::VelocityCurveSet, Action::KitGain,  Action::PadTune,
            Action::PadDecay,         Action::None,    Action::None,
            Action::None,             Action::MasterVolume};
        for (int i = 0; i < 8; ++i) {
            add(knob_id[i], Mode::Play, play_k[i]);
            add(knob_id[i], Mode::Edit, edit_k[i]);
            add(knob_id[i], Mode::Step, step_k[i]);
        }
        // Joystick axes.
        add("joy.x", Mode::Play, Action::None);  // pitch bend (device layer)
        add("joy.y", Mode::Play, Action::None);  // CC1 mod (device layer)
        add("joy.x", Mode::Edit, Action::ScrubCoarse);
        add("joy.y", Mode::Edit, Action::None);
        // Buttons.
        add("btn.octave_up", Mode::Play, Action::OctaveUp);
        add("btn.octave_down", Mode::Play, Action::OctaveDown);
        add("btn.arp", Mode::Play, Action::ArpToggle);
        add("btn.note_repeat", Mode::Play, Action::None);  // note repeat (M4)
        // Pedal.
        add("pedal.sustain", Mode::Play, Action::None);  // CC64 path
        return t;
    }();
    return table;
}

std::vector<std::string> audit_coverage(
    std::vector<device::Control> const& controls) {
    auto const& table = coverage_table();
    std::vector<std::string> dead;
    for (auto const& c : controls) {
        bool found = false;
        for (auto const& e : table) {
            if (e.control_id == c.id) {
                found = true;
                break;
            }
        }
        if (!found) dead.push_back(c.id);
    }
    return dead;
}

}  // namespace ghost_story::mapping
