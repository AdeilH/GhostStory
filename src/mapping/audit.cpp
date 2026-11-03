// addu94 — src/mapping/audit.cpp
#include "mapping/audit.hpp"

#include <algorithm>

namespace addu94::mapping {

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
        // Pads A — chord slots in Play, track select in Edit, step grid.
        for (int row = 1; row <= 2; ++row) {
            for (int col = 1; col <= 4; ++col) {
                std::string const id = "pad.a" + std::to_string(row) +
                                       std::to_string(col);
                add(id, Mode::Play, Action::ChordSlot);
                add(id, Mode::Edit, Action::TrackSelect);
                add(id, Mode::Step, Action::None);
            }
        }
        // Pads B — clip launch in Play, transport/utility toggles in Edit.
        static Action const edit_pad_actions[8] = {
            Action::MetronomeToggle, Action::CountInToggle,
            Action::LoopToggle,      Action::PunchToggle,
            Action::ClipStop,        Action::ClipStop,
            Action::ClipStop,        Action::ClipStop};
        int eb = 0;
        for (int row = 1; row <= 2; ++row) {
            for (int col = 1; col <= 4; ++col) {
                std::string const id = "pad.b" + std::to_string(row) +
                                       std::to_string(col);
                add(id, Mode::Play, Action::ClipLaunch);
                add(id, Mode::Edit, edit_pad_actions[eb++]);
                add(id, Mode::Step, Action::None);
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
            Action::ScrubCoarse, Action::ScrubFine, Action::TempoNudge,
            Action::LoopBarsSet, Action::QuantizeGridSet,
            Action::MasterVolume, Action::ClickLevel, Action::None};
        static Action const step_k[8] = {
            Action::None, Action::None, Action::None, Action::None,
            Action::None, Action::None, Action::None, Action::None};
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

}  // namespace addu94::mapping
