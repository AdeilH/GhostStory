// addu94 — tests/test_recorder.cpp
// M3 gate: record / overdub / undo round-trip via virtual MIDI events.
#include "test_framework.hpp"

#include "music/recorder.hpp"

using addu94::music::Clip;
using addu94::music::ClipEvent;
using addu94::music::Grid;
using addu94::music::Recorder;

TEST(recorder, record_and_undo_roundtrip) {
    Clip clip;
    clip.length_ticks = 96 * 4;  // 1 bar of 4/4
    Recorder rec;
    rec.set_clip(&clip);
    rec.begin_take();

    rec.note_on(0, 0, 60, 100);
    CHECK_EQ(rec.pending_count(), 1u);
    rec.note_off(24, 0, 60);  // quantized to 24 (1/16 grid)
    CHECK_EQ(rec.pending_count(), 0u);
    CHECK_EQ(clip.events.size(), 2u);
    CHECK_EQ(clip.events[0].tick, 0);
    CHECK_EQ(clip.events[0].velocity, 100);
    CHECK(clip.events[0].on);
    CHECK_EQ(clip.events[1].tick, 24);
    CHECK(!clip.events[1].on);

    CHECK(clip.undo_last_take());
    CHECK(clip.events.empty());
    CHECK(!clip.undo_last_take());
}

TEST(recorder, overdub_stack_takes) {
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    rec.set_clip(&clip);

    rec.begin_take();
    rec.note_on(0, 0, 36, 90);
    rec.note_off(48, 0, 36);
    CHECK_EQ(clip.next_take, 1);

    rec.begin_take();  // overdub pass
    rec.note_on(96, 0, 43, 120);
    rec.note_off(96 + 24, 0, 43);
    CHECK_EQ(clip.next_take, 2);
    CHECK_EQ(clip.events.size(), 4u);

    CHECK(clip.undo_last_take());  // drops take 1 only
    CHECK_EQ(clip.events.size(), 2u);
    for (auto const& e : clip.events) CHECK_EQ(e.take, 0);
}

TEST(recorder, loop_wrap_clamps_into_window) {
    Clip clip;
    clip.length_ticks = 96 * 4;  // 384
    Recorder rec;
    rec.set_clip(&clip);
    rec.begin_take();

    // Note started near loop end, released after wrap (tick 400 → 16).
    rec.note_on(380, 0, 48, 64);
    rec.note_off(400, 0, 48);
    // on: 380 quantizes to 384 ≡ 0 (loop start); off: 400 % 384 = 16,
    // then 16 quantizes to 24 (grid). Quantize applies before wrap.
    CHECK_EQ(clip.events[0].tick, 0);
    CHECK_EQ(clip.events[1].tick, 24);
}

TEST(recorder, quantize_off_records_raw) {
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    rec.set_clip(&clip);
    rec.set_grid(Grid::Off);
    rec.begin_take();
    rec.note_on(37, 0, 60, 100);
    rec.note_off(59, 0, 60);
    CHECK_EQ(clip.events[0].tick, 37);  // raw
    CHECK_EQ(clip.events[1].tick, 59);
}

TEST(recorder, flush_closes_stuck_notes_at_loop_end) {
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    rec.set_clip(&clip);
    rec.begin_take();
    rec.note_on(0, 0, 60, 100);
    rec.flush_pending();
    CHECK_EQ(rec.pending_count(), 0u);
    CHECK_EQ(clip.events.size(), 2u);
    CHECK_EQ(clip.events[1].tick, clip.length_ticks - 1);
}

TEST(clip, insert_sorted_keeps_order) {
    Clip clip;
    for (int tick : {96, 0, 48, 24}) {
        clip.insert_sorted(ClipEvent{tick, 0, 60, 100, true, 0});
    }
    CHECK_EQ(clip.events.size(), 4u);
    CHECK_EQ(clip.events[0].tick, 0);
    CHECK_EQ(clip.events[3].tick, 96);
}

TEST_MAIN()
