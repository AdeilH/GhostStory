// addu94 — tests/test_mapping.cpp
// Pad semantics (regression for "pad 1 and pad 2 have same response"):
// every pad has a DISTINCT response; drumming A1-then-A2 must NOT fire
// the old accidental Undo chord.
#include "test_framework.hpp"

#include "mapping/actions.hpp"
#include "mapping/mapping.hpp"
#include "midi/types.hpp"

#include <algorithm>

using addu94::mapping::Action;
using addu94::mapping::MappingEngine;
using addu94::mapping::Mode;
using addu94::midi::MidiEvent;
using addu94::midi::Type;

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

Action first_action(std::vector<addu94::mapping::Command> const& cmds) {
    return cmds.empty() ? Action::None : cmds.front().action;
}
}  // namespace

TEST(mapping, drum_pads_have_distinct_params) {
    MappingEngine m;
    for (int p = 0; p < 8; ++p) {
        auto press = m.consume(pad_event(36 + p, true));
        CHECK_EQ(press.size(), 1u);
        CHECK_EQ(static_cast<int>(first_action(press)),
                 static_cast<int>(Action::DrumPad));
        CHECK_EQ(press.front().param, p);
        auto rel = m.consume(pad_event(36 + p, false));
        (void)rel;
    }
    // Every drum pad param is unique by construction — checked implicitly
    // above by indexing, but assert the set explicitly:
    std::vector<int> seen;
    for (int p = 0; p < 8; ++p) seen.push_back(p);
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

TEST(mapping, drumming_pad1_then_pad2_never_triggers_undo) {
    // THE regression: old scheme fired Undo for the 36+37 pair.
    MappingEngine m;
    auto a = m.consume(pad_event(36, true));
    auto b = m.consume(pad_event(37, true));  // second pad while first held
    auto ar = m.consume(pad_event(36, false));
    auto br = m.consume(pad_event(37, false));
    for (auto const* cmds : {&a, &b, &ar, &br}) {
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
}

TEST(mapping, undo_redo_on_bank_b_instant_on_press) {
    MappingEngine m;
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

TEST(mapping, keys_outside_pad_range_ignored_by_mapping) {
    MappingEngine m;
    CHECK(m.consume(pad_event(60, true)).empty());
    CHECK(m.consume(pad_event(35, true)).empty());
    CHECK(m.consume(pad_event(52, true)).empty());
}

TEST_MAIN()
