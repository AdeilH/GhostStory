// ghost_story — tests/test_arp.cpp
#include "test_framework.hpp"

#include "music/arp.hpp"

#include <set>

using ghost_story::music::Arp;
using ghost_story::music::ArpMode;
using ghost_story::music::NoteRepeat;

TEST(arp, up_walks_sorted_pool) {
    Arp a;
    a.set_mode(ArpMode::Up);
    a.note_on(64);  // E4
    a.note_on(60);  // C4
    a.note_on(67);  // G4
    // pool sorted: 60 64 67
    auto s0 = a.strike(0, 0);
    CHECK_EQ(s0.size(), 1u);
    CHECK_EQ(s0[0], 60);
    a.advance_step();
    CHECK_EQ(a.strike(1, 0).at(0), 64);
    a.advance_step();
    CHECK_EQ(a.strike(2, 0).at(0), 67);
    a.advance_step();
    CHECK_EQ(a.strike(3, 0).at(0), 60);  // wraps
}

TEST(arp, octaves_extend_pool) {
    Arp a;
    a.set_mode(ArpMode::Up);
    a.set_octaves(2);
    a.note_on(60);
    auto s = a.strike(0, 0);
    CHECK_EQ(s[0], 60);
    a.advance_step();
    CHECK_EQ(a.strike(1, 0).at(0), 72);  // +12
}

TEST(arp, updown_bounces_without_end_repeats) {
    Arp a;
    a.set_mode(ArpMode::UpDown);
    a.note_on(60);
    a.note_on(64);
    a.note_on(67);
    std::vector<int> seq;
    for (int i = 0; i < 4; ++i) {
        std::vector<int> s = a.strike(static_cast<std::uint64_t>(i), 0);
        seq.push_back(s[0]);
        a.advance_step();
    }
    // period = 4: 60 64 67 64 (no repeat of 67, no wrap repeat)
    CHECK_EQ(seq[0], 60);
    CHECK_EQ(seq[1], 64);
    CHECK_EQ(seq[2], 67);
    CHECK_EQ(seq[3], 64);
}

TEST(arp, chord_mode_strikes_all) {
    Arp a;
    a.set_mode(ArpMode::Chord);
    a.note_on(60);
    a.note_on(63);
    auto s = a.strike(0, 0);
    CHECK_EQ(s.size(), 2u);
    std::set<int> got(s.begin(), s.end());
    CHECK(got.count(60) == 1);
    CHECK(got.count(63) == 1);
}

TEST(arp, note_off_rebuilds) {
    Arp a;
    a.set_mode(ArpMode::Up);
    a.note_on(60);
    a.note_on(64);
    a.note_off(60);
    CHECK_EQ(a.held_count(), 1u);
    CHECK_EQ(a.strike(0, 0).at(0), 64);
}

TEST(arp, random_stays_in_pool) {
    Arp a;
    a.set_mode(ArpMode::Random);
    a.note_on(60);
    a.note_on(67);
    for (std::uint64_t rng = 0; rng < 100; ++rng) {
        auto s = a.strike(0, rng);
        CHECK(s[0] == 60 || s[0] == 67);
    }
}

TEST(arp, octave_clamp) {
    Arp a;
    a.set_octaves(99);
    a.note_on(60);
    a.set_mode(ArpMode::Chord);
    CHECK_EQ(a.strike(0, 0).size(), 4u);  // clamped to 4 octaves
}

TEST(noterepeat, repeats_held_notes) {
    NoteRepeat nr;
    nr.press(36);
    nr.press(38);
    auto r = nr.repeating();
    CHECK_EQ(r.size(), 2u);
    nr.release(36);
    CHECK_EQ(nr.repeating().size(), 1u);
}

TEST_MAIN()
