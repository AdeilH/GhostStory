// ghost_story — tests/test_recorder.cpp
// M3 gate: record / overdub / undo round-trip via virtual MIDI events —
// extended for the loop machine (per-track clips, raw↔stored identity,
// full-key recording; the old gate silently dropped keys outside 36..43).
#include "test_framework.hpp"

#include "music/recorder.hpp"

using ghost_story::music::Clip;
using ghost_story::music::ClipEvent;
using ghost_story::music::Grid;
using ghost_story::music::Recorder;

// Shorthand: a key note recorded raw==stored (the common synth case).
static bool key_on(Recorder& rec, std::int64_t tick, int note, int vel,
                   Clip& target) {
    return rec.note_on(tick, 0, note, 0, note, vel, target);
}

TEST(recorder, record_and_undo_roundtrip) {
    Clip clip;
    clip.length_ticks = 96 * 4;  // 1 bar of 4/4
    Recorder rec;

    CHECK(key_on(rec, 0, 60, 100, clip));
    CHECK_EQ(rec.pending_count(), 1u);
    CHECK(rec.note_off(24, 0, 60) != nullptr);  // quantized to 24 (1/16)
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

    CHECK(key_on(rec, 0, 36, 90, clip));
    rec.note_off(48, 0, 36);
    CHECK_EQ(clip.next_take, 1);

    CHECK(key_on(rec, 96, 43, 120, clip));  // overdub pass
    rec.note_off(96 + 24, 0, 43);
    CHECK_EQ(clip.next_take, 2);
    CHECK_EQ(clip.events.size(), 4u);

    CHECK(clip.undo_last_take());  // drops take 1 only
    CHECK_EQ(clip.events.size(), 2u);
    for (auto const& e : clip.events) CHECK_EQ(e.take, 0);
}

TEST(recorder, undo_redo_restores_take) {
    // Regression ("BANK B doesn't work at all"): B2/Redo emitted its
    // action but the engine had no case for it AND Clip had no redo stack
    // — undo was destructive with no way back.
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;

    CHECK(key_on(rec, 0, 36, 90, clip));
    rec.note_off(24, 0, 36);
    CHECK_EQ(clip.next_take, 1);
    auto const saved = clip.events;

    CHECK(clip.undo_last_take());
    CHECK(clip.events.empty());
    CHECK(!clip.undo_last_take());

    CHECK(clip.redo_last_take());
    CHECK_EQ(clip.events.size(), saved.size());
    CHECK_EQ(clip.next_take, 1);
    for (std::size_t i = 0; i < saved.size(); ++i) {
        CHECK_EQ(clip.events[i].tick, saved[i].tick);
        CHECK_EQ(clip.events[i].note, saved[i].note);
        CHECK_EQ(clip.events[i].take, saved[i].take);
        CHECK_EQ(clip.events[i].on, saved[i].on);
    }
    CHECK(!clip.redo_last_take());
}

TEST(recorder, redo_stack_multilevel_and_invalidation) {
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;

    CHECK(key_on(rec, 0, 36, 90, clip));
    rec.note_off(24, 0, 36);  // take 0: 2 events at ticks 0/24
    CHECK(key_on(rec, 96, 37, 90, clip));
    rec.note_off(96 + 24, 0, 37);  // take 1: 2 events at ticks 96/120
    CHECK_EQ(clip.events.size(), 4u);

    CHECK(clip.undo_last_take());  // drops take 1
    CHECK_EQ(clip.events.size(), 2u);
    CHECK(clip.undo_last_take());  // drops take 0
    CHECK(clip.events.empty());

    CHECK(clip.redo_last_take());  // restores take 0
    CHECK_EQ(clip.events.size(), 2u);
    CHECK_EQ(clip.next_take, 1);
    CHECK_EQ(clip.events[0].tick, 0);
    CHECK_EQ(clip.events[0].take, 0);
    CHECK(clip.redo_last_take());  // restores take 1, re-sorted by tick
    CHECK_EQ(clip.events.size(), 4u);
    CHECK_EQ(clip.next_take, 2);
    CHECK_EQ(clip.events[2].tick, 96);
    CHECK(!clip.redo_last_take());

    // New recording invalidates redo: the fresh take reuses the stashed
    // take index, so restoring it would interleave two takes under one
    // number. (No explicit begin_take anymore — the first note of the new
    // session invalidates.)
    CHECK(clip.undo_last_take());
    CHECK_EQ(clip.events.size(), 2u);
    CHECK(key_on(rec, 48, 38, 90, clip));
    rec.note_off(48 + 24, 0, 38);
    CHECK_EQ(clip.next_take, 2);
    CHECK(!clip.redo_last_take());
    CHECK_EQ(clip.events.size(), 4u);
}

TEST(recorder, loop_wrap_clamps_into_window) {
    Clip clip;
    clip.length_ticks = 96 * 4;  // 384
    Recorder rec;

    // Note started near loop end, released after wrap (tick 400 → 16).
    CHECK(key_on(rec, 380, 48, 64, clip));
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
    rec.set_grid(Grid::Off);
    CHECK(key_on(rec, 37, 60, 100, clip));
    rec.note_off(59, 0, 60);
    CHECK_EQ(clip.events[0].tick, 37);  // raw
    CHECK_EQ(clip.events[1].tick, 59);
}

TEST(recorder, flush_closes_stuck_notes_at_loop_end) {
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    CHECK(key_on(rec, 0, 60, 100, clip));
    rec.flush_pending();
    CHECK_EQ(rec.pending_count(), 0u);
    CHECK_EQ(clip.events.size(), 2u);
    CHECK_EQ(clip.events[1].tick, clip.length_ticks - 1);
}

TEST(recorder, full_key_range_records) {
    // Regression (user report: "I can't record the instruments at all"):
    // the old gate only accepted notes 36..43 — a key played in any normal
    // octave silently recorded nothing. The recorder itself is window-free;
    // this pins that a C5 (72) and a top B7 (119) both commit.
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    CHECK(key_on(rec, 0, 72, 100, clip));
    rec.note_off(24, 0, 72);
    CHECK(key_on(rec, 48, 119, 90, clip));
    rec.note_off(72, 0, 119);
    CHECK_EQ(clip.events.size(), 4u);
    CHECK_EQ(clip.events[0].note, 72);
    CHECK_EQ(clip.events[2].note, 119);
}

TEST(recorder, stored_identity_differs_from_raw) {
    // Keys recorded through the engine carry the SOUNDING identity
    // (post-octave/pad transform + the track's own channel) for playback,
    // while pending matching uses the RAW device (channel, note) — an
    // octave-shifted off must still close its on.
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    // Raw key ch0 note 48 sounds as track-Bass channel 2 note 60.
    CHECK(rec.note_on(0, 0, 48, 2, 60, 100, clip));
    CHECK_EQ(rec.pending_count(), 1u);
    Clip* committed = rec.note_off(24, 0, 48);  // raw off matches
    CHECK(committed == &clip);
    CHECK_EQ(clip.events.size(), 2u);
    CHECK_EQ(clip.events[0].channel, 2);  // stored identity
    CHECK_EQ(clip.events[0].note, 60);
    CHECK_EQ(clip.events[1].channel, 2);
    CHECK_EQ(clip.events[1].note, 60);
    // An off for the STORED note (wrongly routed) matches nothing.
    CHECK(key_on(rec, 96, 48, 100, clip));
    CHECK(rec.note_off(120, 0, 60) == nullptr);
    CHECK_EQ(rec.pending_count(), 1u);
    CHECK(rec.note_off(120, 0, 48) != nullptr);
}

TEST(recorder, pending_binds_its_own_clip) {
    // Loop machine: each pending belongs to the clip it landed in —
    // switching the recording target mid-note cannot misroute the off.
    Clip keys_clip;
    keys_clip.length_ticks = 96 * 4;
    Clip drums_clip;
    drums_clip.length_ticks = 96 * 4;
    Recorder rec;

    CHECK(rec.note_on(0, 9, 36, 9, 36, 100, drums_clip));  // pad hit
    CHECK(key_on(rec, 24, 60, 100, keys_clip));            // key held
    // Pad off closes into the DRUMS clip even though keys are pending too.
    CHECK(rec.note_off(48, 9, 36) == &drums_clip);
    CHECK_EQ(drums_clip.events.size(), 2u);
    CHECK_EQ(keys_clip.events.size(), 0u);
    // Key off closes into the KEYS clip.
    CHECK(rec.note_off(72, 0, 60) == &keys_clip);
    CHECK_EQ(keys_clip.events.size(), 2u);
    // Flush (stop) closes the rest into their own clips — none here.
    rec.flush_pending();
    CHECK_EQ(keys_clip.events.size(), 2u);
    CHECK_EQ(drums_clip.events.size(), 2u);
}

TEST(recorder, event_cap_rejects) {
    Clip clip;
    clip.length_ticks = 96 * 4;
    Recorder rec;
    // Fill to the ceiling with committed events (2 per note).
    for (int i = 0;
         i * 2 < static_cast<int>(ghost_story::music::kMaxClipEvents);
         ++i) {
        CHECK(key_on(rec, i, i % 128, 100, clip));
        CHECK(rec.note_off(i + 1, 0, i % 128) != nullptr);
    }
    CHECK_EQ(clip.events.size(), ghost_story::music::kMaxClipEvents);
    // The next note is rejected — the RT snapshot array stays bounded.
    CHECK(!key_on(rec, 0, 60, 100, clip));
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

// --- take boxes: the [rec 1] [rec 2] blocks the arrangement draws -------

TEST(clip, take_boxes_disjoint_spans_stay_separate) {
    // The user's ask verbatim: "[rec 1] [rec 2] — we want both".
    Clip clip;
    clip.place_take_span(0, 0, 192);     // rec 1: bars 0-2
    clip.place_take_span(1, 768, 192);   // rec 2: bars 8-10
    CHECK_EQ(clip.boxes.size(), 2u);
    CHECK_EQ(clip.boxes[0].take, 0);
    CHECK_EQ(clip.boxes[0].start_tick, 0);
    CHECK_EQ(clip.boxes[1].take, 1);
    CHECK_EQ(clip.boxes[1].start_tick, 768);
}

TEST(clip, take_boxes_overlap_merges_earliest_take) {
    // An overdub punched into an existing take extends its box instead of
    // stacking a twin block; the box keeps the earliest take (undo of the
    // newer take must not erase the older take's visual).
    Clip clip;
    clip.place_take_span(0, 0, 192);
    clip.place_take_span(1, 96, 192);  // overlaps rec 1's box
    CHECK_EQ(clip.boxes.size(), 1u);
    CHECK_EQ(clip.boxes[0].take, 0);
    CHECK_EQ(clip.boxes[0].start_tick, 0);
    CHECK_EQ(clip.boxes[0].length_ticks, 288);  // union 0..288
}

TEST(clip, take_boxes_grow_while_the_take_rolls) {
    // take_cover feeds the same take's growing span every drain — the box
    // grows in place, never duplicates.
    Clip clip;
    clip.place_take_span(0, 0, 96);
    clip.place_take_span(0, 0, 192);
    clip.place_take_span(0, 0, 384);
    CHECK_EQ(clip.boxes.size(), 1u);
    CHECK_EQ(clip.boxes[0].length_ticks, 384);
}

TEST(clip, undo_redo_carries_the_box) {
    Clip clip;
    clip.place_take_span(0, 0, 192);
    clip.insert_sorted(ClipEvent{48, 0, 60, 100, true, 0});
    clip.insert_sorted(ClipEvent{96, 0, 60, 0, false, 0});
    clip.next_take = 1;  // the recorder bumps it at commit
    CHECK(clip.undo_last_take());
    CHECK(clip.boxes.empty());
    CHECK(clip.redo_last_take());
    CHECK_EQ(clip.boxes.size(), 1u);
    CHECK_EQ(clip.boxes[0].start_tick, 0);
    CHECK_EQ(clip.boxes[0].length_ticks, 192);
}

TEST(clip, undo_of_merged_take_keeps_earlier_box) {
    // Take 1 merged into take 0's box: undoing take 1 drops its events but
    // the box (earliest take's span) stays.
    Clip clip;
    clip.place_take_span(0, 0, 192);
    clip.place_take_span(1, 96, 96);  // merges into box(take 0)
    clip.insert_sorted(ClipEvent{120, 0, 62, 100, true, 1});
    clip.insert_sorted(ClipEvent{140, 0, 62, 0, false, 1});
    clip.next_take = 2;
    CHECK(clip.undo_last_take());
    CHECK_EQ(clip.boxes.size(), 1u);  // take 0's span survives
    CHECK_EQ(clip.boxes[0].take, 0);
}

TEST(clip, prune_drops_noteless_boxes) {
    // The transport places a take's box before the first note lands; a
    // take that never played must not leave a ghost block.
    Clip clip;
    clip.place_take_span(0, 384, 96);  // provisional, never recorded
    clip.insert_sorted(ClipEvent{48, 0, 60, 100, true, 1});
    clip.insert_sorted(ClipEvent{96, 0, 60, 0, false, 1});
    clip.place_take_span(1, 0, 96);
    clip.prune_empty_boxes();
    CHECK_EQ(clip.boxes.size(), 1u);
    CHECK_EQ(clip.boxes[0].take, 1);
}

TEST(clip, clear_take_removes_one_take_only) {
    Clip clip;
    clip.place_take_span(0, 0, 192);
    clip.place_take_span(1, 768, 192);
    clip.insert_sorted(ClipEvent{48, 0, 60, 100, true, 0});
    clip.insert_sorted(ClipEvent{800, 0, 62, 100, true, 1});
    clip.clear_take(1);
    CHECK_EQ(clip.events.size(), 1u);  // take 0's note survives
    CHECK_EQ(clip.events[0].take, 0);
    CHECK_EQ(clip.boxes.size(), 1u);
    CHECK_EQ(clip.boxes[0].take, 0);
}

TEST_MAIN()
