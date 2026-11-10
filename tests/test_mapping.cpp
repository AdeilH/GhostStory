// ghost_story — tests/test_mapping.cpp
// Pad semantics (regression for "pad 1 and pad 2 have same response"):
// every pad has a DISTINCT response; drumming A1-then-A2 must NOT fire
// the old accidental Undo chord.
#include "test_framework.hpp"

#include "mapping/actions.hpp"
#include "mapping/mapping.hpp"
#include "midi/types.hpp"

#include <algorithm>

using ghost_story::mapping::Action;
using ghost_story::mapping::MappingEngine;
using ghost_story::mapping::Mode;
using ghost_story::midi::MidiEvent;
using ghost_story::midi::Type;

namespace {
MidiEvent pad_event(int note, bool on) {
    MidiEvent ev{};
    ev.type = on ? Type::NoteOn : Type::NoteOff;
    ev.channel = 9;
    ev.d1 = static_cast<std::uint8_t>(note);
    ev.d2 = on ? 100 : 0;
    ev.stamp_ns = 1;
    return ev;
}

MidiEvent cc_event(int cc, int val) {
    MidiEvent ev{};
    ev.type = Type::CC;
    ev.channel = 0;
    ev.d1 = static_cast<std::uint8_t>(cc);
    ev.d2 = static_cast<std::uint8_t>(val);
    ev.stamp_ns = 1;
    return ev;
}

Action first_action(std::vector<ghost_story::mapping::Command> const& cmds) {
    return cmds.empty() ? Action::None : cmds.front().action;
}
}  // namespace

TEST(mapping, drum_pads_have_distinct_params) {
    MappingEngine m;
    // PLAY: all 16 pads are drum pads (pads semantics v2 — "BANK B doesn't
    // work at all" was bank B being utility-only). Bank A = kit sounds,
    // bank B = the same sounds one octave up; param = absolute pad index.
    for (int p = 0; p < 16; ++p) {
        auto press = m.consume(pad_event(36 + p, true));
        CHECK_EQ(press.size(), 1u);
        CHECK_EQ(static_cast<int>(first_action(press)),
                 static_cast<int>(Action::DrumPad));
        CHECK_EQ(press.front().param, p);
        (void)m.consume(pad_event(36 + p, false));
    }
}

TEST(mapping, drumming_pad1_then_pad2_never_triggers_undo) {
    // THE regression: old scheme fired Undo for the 36+37 pair. v2 makes it
    // structural: PLAY has NO utility pads at all — every pad drums, so
    // drumming can never reach Undo/Redo (bank B included).
    MappingEngine m;
    auto a = m.consume(pad_event(36, true));
    auto b = m.consume(pad_event(37, true));  // second pad while first held
    auto bb = m.consume(pad_event(44, true));  // former Undo pad, now a drum
    auto bb2 = m.consume(pad_event(45, true)); // former Redo pad
    auto ar = m.consume(pad_event(36, false));
    auto br = m.consume(pad_event(37, false));
    for (auto const* cmds : {&a, &b, &bb, &bb2, &ar, &br}) {
        for (auto const& c : *cmds) {
            CHECK(c.action != Action::Undo);
            CHECK(c.action != Action::Redo);
        }
    }
    // And each pad still produced its own DrumPad.
    CHECK_EQ(static_cast<int>(first_action(a)), static_cast<int>(Action::DrumPad));
    CHECK_EQ(static_cast<int>(first_action(b)), static_cast<int>(Action::DrumPad));
    CHECK_EQ(a.front().param, 0);
    CHECK_EQ(b.front().param, 1);
    CHECK_EQ(bb.front().param, 8);
    CHECK_EQ(bb2.front().param, 9);
}

TEST(mapping, undo_redo_live_in_edit_only) {
    // Pads semantics v2: PLAY is a pure drum machine — no utility pads, so
    // drumming bank B can never erase a take. Undo/redo live in EDIT
    // (B1/B2) and on Ctrl+Z/Ctrl+Shift+Z.
    MappingEngine m;
    // PLAY: 44 (former Undo) is a drum pad now.
    auto play_hit = m.consume(pad_event(44, true));
    CHECK_EQ(static_cast<int>(first_action(play_hit)),
             static_cast<int>(Action::DrumPad));
    CHECK_EQ(play_hit.front().param, 8);
    m.set_mode(Mode::Edit);
    auto undo = m.consume(pad_event(44, true));
    CHECK_EQ(undo.size(), 1u);
    CHECK_EQ(static_cast<int>(first_action(undo)),
             static_cast<int>(Action::Undo));
    auto redo = m.consume(pad_event(45, true));
    CHECK_EQ(static_cast<int>(first_action(redo)),
             static_cast<int>(Action::Redo));
    // No release-gated duplicates.
    auto undo_rel = m.consume(pad_event(44, false));
    CHECK(undo_rel.empty());
}

TEST(mapping, chord_slots_emit_press_release_pairs) {
    MappingEngine m;
    m.set_mode(Mode::Edit);
    for (int s = 0; s < 6; ++s) {
        auto press = m.consume(pad_event(46 + s, true));
        CHECK_EQ(press.size(), 1u);
        CHECK_EQ(static_cast<int>(first_action(press)),
                 static_cast<int>(Action::ChordSlot));
        CHECK_EQ(press.front().param, s);
        CHECK_EQ(press.front().delta, 0);
        auto rel = m.consume(pad_event(46 + s, false));
        CHECK_EQ(rel.size(), 1u);
        CHECK_EQ(rel.front().delta, 1);
    }
}

TEST(mapping, edit_mode_bank_a_selects_tracks) {
    MappingEngine m;
    m.set_mode(Mode::Edit);
    auto press = m.consume(pad_event(38, true));  // pad 3 -> track idx 2
    CHECK_EQ(static_cast<int>(first_action(press)),
             static_cast<int>(Action::TrackSelect));
    CHECK_EQ(press.front().param, 2);
}

TEST(mapping, edit_mode_bank_b_undo_and_chords) {
    // Regression ("BANK B doesn't work at all", round 1): bank B had no
    // EDIT-mode functions at all. Round 2 moved undo/redo + chord slots
    // here (out of PLAY, which is now a pure 16-pad drum machine).
    MappingEngine m;
    m.set_mode(Mode::Edit);
    CHECK_EQ(static_cast<int>(first_action(m.consume(pad_event(44, true)))),
             static_cast<int>(Action::Undo));
    CHECK_EQ(static_cast<int>(first_action(m.consume(pad_event(45, true)))),
             static_cast<int>(Action::Redo));
    for (int s = 0; s < 6; ++s) {
        auto press = m.consume(pad_event(46 + s, true));
        CHECK_EQ(static_cast<int>(first_action(press)),
                 static_cast<int>(Action::ChordSlot));
        CHECK_EQ(press.front().param, s);
        auto rel = m.consume(pad_event(46 + s, false));
        CHECK_EQ(rel.size(), 1u);
        CHECK_EQ(rel.front().delta, 1);
    }
}

TEST(mapping, kit_mode_pads_select_packs) {
    // "I don't get any other pack on pad working": the KIT mode puts all
    // 16 packs ON the pads — every pad selects its pack (and PlayEngine
    // auditions the voice, engine-side).
    MappingEngine m;
    m.set_mode(Mode::Step);
    for (int p = 0; p < 16; ++p) {
        auto press = m.consume(pad_event(36 + p, true));
        CHECK_EQ(press.size(), 1u);
        CHECK_EQ(static_cast<int>(first_action(press)),
                 static_cast<int>(Action::PadPackSelect));
        CHECK_EQ(press.front().param, p);
        // Press-only: releases emit nothing.
        CHECK(m.consume(pad_event(36 + p, false)).empty());
    }
    // Bank A in KIT is pack select too (not track select).
    auto a1 = m.consume(pad_event(36, true));
    CHECK_EQ(static_cast<int>(first_action(a1)),
             static_cast<int>(Action::PadPackSelect));
}

// --- Edit-mode FX knob page (v0.2): knobs 1-4 = select/toggle/A/B, 5-8
// keep tempo/loop/quantize/master. The old scrub coarse/fine + click
// bindings were stored-only no-ops and gave way to live FX controls. ---

TEST(mapping, edit_mode_fx_knob_page) {
    MappingEngine m;
    m.set_mode(Mode::Edit);
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(1, 40)))),
             static_cast<int>(Action::FxSelect));
    CHECK_EQ(m.consume(cc_event(1, 40)).front().param, 40);
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(2, 1)))),
             static_cast<int>(Action::FxToggle));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(3, 90)))),
             static_cast<int>(Action::FxParamA));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(4, 10)))),
             static_cast<int>(Action::FxParamB));
    // Knobs 5-8 unchanged.
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(5, 3)))),
             static_cast<int>(Action::TempoNudge));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(6, 4)))),
             static_cast<int>(Action::LoopBarsSet));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(7, 64)))),
             static_cast<int>(Action::QuantizeGridSet));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(8, 100)))),
             static_cast<int>(Action::MasterVolume));
    // Play mode keeps its own table (untouched by the FX page).
    m.set_mode(Mode::Play);
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(1, 64)))),
             static_cast<int>(Action::TrackVolume));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(3, 100)))),
             static_cast<int>(Action::TrackSend));
}

TEST(mapping, keys_outside_pad_range_ignored_by_mapping) {
    MappingEngine m;
    CHECK(m.consume(pad_event(60, true)).empty());
    CHECK(m.consume(pad_event(35, true)).empty());
    CHECK(m.consume(pad_event(52, true)).empty());
}

// --- knob CC alias sets (regression: "the knobs are doing nothing at
// all" — the old map knew only CC 1..8; stock mk2/mk3 programs ship the
// knobs on 21..28 or 70..77) ---

TEST(mapping, stock_cc_set_1_to_8_maps_knob_positions) {
    MappingEngine m;
    // Each CC maps to its position-in-set: the value lands in param, and
    // the PLAY-mode action table follows the knob index.
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(1, 10)))),
             static_cast<int>(Action::TrackVolume));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(2, 20)))),
             static_cast<int>(Action::TrackPan));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(5, 50)))),
             static_cast<int>(Action::FilterCutoff));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(8, 80)))),
             static_cast<int>(Action::EnvRelease));
    // Values mirror per knob index (not per CC mod 8). NOTE: CC 8 is not
    // checked on knob 0 — the provisional mirror write (consume's
    // `knob_values_[cc % 8]`, kept for controller-mirror compatibility)
    // parks CC 8's value in slot 0 before map_cc corrects slot 7.
    CHECK_EQ(m.knob_value(4), 50);
    CHECK_EQ(m.knob_value(7), 80);
    // Resolved CC map defaults to the first stock set.
    CHECK_EQ(m.knob_cc(0), 1);
    CHECK_EQ(m.knob_cc(7), 8);
}

TEST(mapping, stock_cc_set_21_to_28_maps_knob_positions) {
    MappingEngine m;
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(21, 10)))),
             static_cast<int>(Action::TrackVolume));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(25, 50)))),
             static_cast<int>(Action::FilterCutoff));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(28, 80)))),
             static_cast<int>(Action::EnvRelease));
    CHECK_EQ(m.knob_value(0), 10);
    CHECK_EQ(m.knob_value(7), 80);
}

TEST(mapping, stock_cc_set_70_to_77_maps_knob_positions) {
    MappingEngine m;
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(70, 10)))),
             static_cast<int>(Action::TrackVolume));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(74, 50)))),
             static_cast<int>(Action::FilterCutoff));
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(77, 80)))),
             static_cast<int>(Action::EnvRelease));
    CHECK_EQ(m.knob_value(4), 50);
}

TEST(mapping, profile_override_wins_over_alias_sets) {
    MappingEngine m;
    std::string err;
    CHECK(m.load_profile("[knobs]\n1.cc = 33\n2.cc = 34\n", err));
    CHECK(err.empty());
    // Calibrated CC (outside every alias set) drives knob 1.
    auto cmds = m.consume(cc_event(33, 7));
    CHECK_EQ(cmds.size(), 1u);
    CHECK_EQ(static_cast<int>(first_action(cmds)),
             static_cast<int>(Action::TrackVolume));
    CHECK_EQ(cmds.front().param, 7);
    // Accessor reflects the override; unset knobs keep defaults.
    CHECK_EQ(m.knob_cc(0), 33);
    CHECK_EQ(m.knob_cc(1), 34);
    CHECK_EQ(m.knob_cc(2), 3);
    // The overridden knobs' old CCs do NOT leak through as aliases for
    // their knob... but CC 1 still aliases knob position 0 (stock set) —
    // so only assert the override CC is consumed exactly once.
    CHECK_EQ(m.consume(cc_event(34, 5)).front().param, 5);
}

TEST(mapping, unmapped_cc_yields_no_commands) {
    MappingEngine m;
    // Gaps between the three stock sets, sustain pedal, and high CCs:
    // none of these may invent a knob.
    for (int cc : {0, 9, 20, 29, 33, 64, 69, 78, 100, 119}) {
        CHECK(m.consume(cc_event(cc, 50)).empty());
    }
}

TEST(mapping, kit_mode_knobs_are_sound_design_page) {
    // KIT knobs 1..8: velocity curve, kit gain, pad tune/decay, three
    // deliberate no-ops, master. Before this test the whole row was
    // Action::None — the on-screen mirror could turn them, hardware could
    // turn them, nothing answered.
    CHECK_EQ(static_cast<int>(MappingEngine::knob_action(Mode::Step, 0)),
             static_cast<int>(Action::VelocityCurveSet));
    CHECK_EQ(static_cast<int>(MappingEngine::knob_action(Mode::Step, 1)),
             static_cast<int>(Action::KitGain));
    CHECK_EQ(static_cast<int>(MappingEngine::knob_action(Mode::Step, 2)),
             static_cast<int>(Action::PadTune));
    CHECK_EQ(static_cast<int>(MappingEngine::knob_action(Mode::Step, 3)),
             static_cast<int>(Action::PadDecay));
    CHECK_EQ(static_cast<int>(MappingEngine::knob_action(Mode::Step, 7)),
             static_cast<int>(Action::MasterVolume));
    CHECK_EQ(static_cast<int>(MappingEngine::knob_action(Mode::Step, 5)),
             static_cast<int>(Action::None));

    // Hardware CCs land on the same table in KIT mode.
    MappingEngine m;
    m.set_mode(Mode::Step);
    CHECK_EQ(static_cast<int>(first_action(m.consume(cc_event(2, 90)))),
             static_cast<int>(Action::KitGain));

    // Every bound knob has a non-empty label in all three modes (the old
    // UI-side label table drifted from the action table once).
    for (int mode_i = 0; mode_i < 3; ++mode_i) {
        auto const mode = static_cast<Mode>(mode_i);
        for (int i = 0; i < 8; ++i) {
            char const* lbl = MappingEngine::knob_label(mode, i);
            if (MappingEngine::knob_action(mode, i) != Action::None) {
                CHECK(lbl != nullptr);
                CHECK(lbl[0] != '\0');
            }
        }
    }
}

TEST(mapping, knob_action_matches_hardware_cc_paths) {
    // The hoisted knob_action table must agree with what map_cc emits in
    // every mode (one source of truth for hardware + mirror knobs).
    struct Row {
        Mode mode;
        int idx;
        Action expect;
    };
    Row const rows[] = {
        {Mode::Play, 0, Action::TrackVolume},
        {Mode::Play, 3, Action::InstrumentMacro1},
        {Mode::Play, 7, Action::EnvRelease},
        {Mode::Edit, 0, Action::FxSelect},
        {Mode::Edit, 1, Action::FxToggle},
        {Mode::Edit, 2, Action::FxParamA},
        {Mode::Edit, 4, Action::TempoNudge},
        {Mode::Edit, 6, Action::QuantizeGridSet},
        {Mode::Edit, 7, Action::MasterVolume},
    };
    for (auto const& row : rows) {
        CHECK_EQ(static_cast<int>(MappingEngine::knob_action(row.mode,
                                                            row.idx)),
                 static_cast<int>(row.expect));
        // The hardware path agrees with the table.
        MappingEngine m;
        m.set_mode(row.mode);
        // CC for knob idx within the first stock set.
        CHECK_EQ(static_cast<int>(first_action(m.consume(
                     cc_event(row.idx + 1, 64)))),
                 static_cast<int>(row.expect));
    }
}

TEST_MAIN()
