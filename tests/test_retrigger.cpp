// ghost_story — tests/test_retrigger.cpp
// Keys play-once guard (user report: "played multiple times on pressing"):
// a held key must sound exactly once while ONCE mode is active — hardware
// arpeggiator re-fires (extra NoteOns without an intervening NoteOff) are
// suppressed — and every note-off form must release the key again.
#include "test_framework.hpp"

#include "music/retrigger.hpp"

using ghost_story::music::RetriggerGuard;

namespace {
constexpr std::uint64_t kNs = 1ULL * 1'000'000'000ULL;  // 1 s in ns
}  // namespace

TEST(retrigger, first_press_allowed_repeat_suppressed) {
    RetriggerGuard g;
    CHECK(g.allow(0, 69, true, 0 * kNs));          // press
    CHECK(!g.allow(0, 69, true, 0 * kNs + 10'000));  // arp re-fire: blocked
    CHECK(!g.allow(0, 69, true, 1 * kNs));           // still held: blocked
    CHECK(g.allow(0, 70, true, 0 * kNs));          // other note unaffected
    CHECK(g.allow(1, 69, true, 0 * kNs));          // other channel unaffected
}

TEST(retrigger, note_off_releases_key) {
    RetriggerGuard g;
    CHECK(g.allow(0, 60, true, 0 * kNs));
    CHECK(!g.allow(0, 60, true, 0 * kNs + 1'000));  // held
    CHECK(g.allow(0, 60, false, 0 * kNs + 2'000));  // NoteOff releases
    CHECK(g.allow(0, 60, true, 0 * kNs + 3'000));   // re-press sounds
}

TEST(retrigger, velocity_zero_noteon_is_a_note_off) {
    // MIDI: NoteOn with velocity 0 means NoteOff. The guard must clear the
    // mark on it, or a 0-velocity pulse would stick the key forever.
    RetriggerGuard g;
    CHECK(g.allow(0, 60, true, 0 * kNs));
    CHECK(!g.allow(0, 60, true, 0 * kNs + 1'000));
    CHECK(g.allow(0, 60, false, 0 * kNs + 2'000));  // vel-0 form
    CHECK(g.allow(0, 60, true, 0 * kNs + 3'000));
}

TEST(retrigger, stale_mark_self_expires) {
    // Latch/all-notes-off unlatching never sends per-note NoteOffs; a mark
    // older than kStuckNoteNs must expire instead of silencing the key.
    RetriggerGuard g;
    CHECK(g.allow(0, 60, true, 0 * kNs));
    CHECK(!g.allow(0, 60, true, 4 * kNs));  // inside the window: blocked
    CHECK(g.allow(0, 60, true, 6 * kNs));   // past 5 s: mark expired
}

TEST(retrigger, repeat_mode_passes_everything) {
    RetriggerGuard g;
    g.set_active(false);
    CHECK(g.allow(0, 60, true, 0 * kNs));
    CHECK(g.allow(0, 60, true, 0 * kNs + 1'000));  // re-fire passes
    CHECK(g.allow(0, 60, true, 0 * kNs + 2'000));
}

TEST(retrigger, clear_releases_everything) {
    RetriggerGuard g;
    CHECK(g.allow(0, 60, true, 0 * kNs));
    CHECK(g.allow(3, 72, true, 0 * kNs));
    CHECK(!g.allow(0, 60, true, 0 * kNs + 1'000));
    g.clear();
    CHECK(g.allow(0, 60, true, 0 * kNs + 2'000));
    CHECK(g.allow(3, 72, true, 0 * kNs + 2'000));
}

TEST(retrigger, out_of_range_never_blocks) {
    RetriggerGuard g;
    CHECK(g.allow(-1, 60, true, 0));
    CHECK(g.allow(0, -5, true, 0));
    CHECK(g.allow(0, 128, true, 0));
    CHECK(g.allow(16, 60, true, 0));
}

TEST_MAIN()
