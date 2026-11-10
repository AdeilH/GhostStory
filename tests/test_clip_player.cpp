// ghost_story — tests/test_clip_player.cpp
// The RT clip sequencer (pure scheduling math) + the end-to-end loop
// machine gate: record through the real event queue, then verify the take
// is AUDIBLE through the real render callback. That assertion is the
// regression for "it only plays metronome when I am trying to play" —
// clip playback did not exist at all before the ClipPlayer landed.
#include "test_framework.hpp"

#include "app/play_engine.hpp"
#include "music/clip_player.hpp"

#include <vector>

using ghost_story::app::PlayEngine;
using ghost_story::app::TrackKind;
using ghost_story::mapping::Action;
using ghost_story::music::Clip;
using ghost_story::music::ClipEvent;
using ghost_story::music::ClipPlayer;

namespace {

struct Fire {
    int channel;
    int note;
    int velocity;
    bool on;
};

// 4-bar clip at 120 bpm / 48 kHz: 384 ticks × 250 samples/tick.
constexpr double kSPT = 250.0;  // samples per tick
constexpr std::uint64_t kLoopSamples = 384 * 250;

Clip make_clip(std::int64_t on_tick, std::int64_t off_tick, int note = 60) {
    Clip c;
    c.length_ticks = 96 * 4;
    c.insert_sorted(ClipEvent{on_tick, 0, static_cast<std::uint8_t>(note),
                              100, true, 0});
    c.insert_sorted(ClipEvent{off_tick, 0, static_cast<std::uint8_t>(note),
                              0, false, 0});
    return c;
}

std::vector<Fire> render_window(ClipPlayer& p, std::uint64_t start,
                                std::uint32_t frames) {
    std::vector<Fire> out;
    p.render(start, frames,
             [&](int channel, int note, int velocity, bool on) noexcept {
                 out.push_back({channel, note, velocity, on});
             });
    return out;
}

}  // namespace

TEST(clip_player, fires_events_inside_the_window) {
    ClipPlayer p;
    Clip const clip = make_clip(0, 48);
    p.publish(clip, 120.0, 48000.0);
    CHECK_EQ(p.published_count(), 2u);

    // Buffer starting at sample 0: the note-on at offset 0 fires, the off
    // at 48*250=12000 does not.
    auto fires = render_window(p, 0, 1000);
    CHECK_EQ(fires.size(), 1u);
    CHECK(fires[0].on);
    CHECK_EQ(fires[0].note, 60);
}

TEST(clip_player, off_fires_in_its_own_window) {
    ClipPlayer p;
    p.publish(make_clip(0, 48), 120.0, 48000.0);
    // Straddle the off at 12000 samples.
    auto fires = render_window(p, 11990, 20);
    CHECK_EQ(fires.size(), 1u);
    CHECK(!fires[0].on);
    CHECK_EQ(fires[0].note, 60);
}

TEST(clip_player, loop_wrap_refires) {
    ClipPlayer p;
    p.publish(make_clip(0, 48), 120.0, 48000.0);
    // Window that crosses the loop end: the wrap segment re-fires the on.
    auto fires = render_window(p, kLoopSamples - 100, 200);
    CHECK_EQ(fires.size(), 1u);
    CHECK(fires[0].on);
}

TEST(clip_player, release_all_closes_held_notes) {
    ClipPlayer p;
    // Held the whole loop: on @0, off @ the last tick.
    p.publish(make_clip(0, 96 * 4 - 1), 120.0, 48000.0);
    auto fires = render_window(p, 0, 1000);
    CHECK_EQ(fires.size(), 1u);
    CHECK(fires[0].on);

    std::vector<Fire> released;
    p.release_all([&](int channel, int note, int velocity, bool on) noexcept {
        released.push_back({channel, note, velocity, on});
    });
    CHECK_EQ(released.size(), 1u);
    CHECK(!released[0].on);
    CHECK_EQ(released[0].note, 60);
    // Second release: nothing left sounding.
    released.clear();
    p.release_all([&](int channel, int note, int velocity, bool on) noexcept {
        released.push_back({channel, note, velocity, on});
    });
    CHECK(released.empty());
}

TEST(clip_player, tempo_change_republish_moves_offsets) {
    ClipPlayer p;
    p.publish(make_clip(0, 48), 120.0, 48000.0);  // 250 s/tick → off @ 12000
    // At 240 bpm the same tick is 125 samples/tick → off @ 6000.
    p.publish(make_clip(0, 48), 240.0, 48000.0);
    CHECK(render_window(p, 11990, 20).empty());
    auto fires = render_window(p, 5990, 20);
    CHECK_EQ(fires.size(), 1u);
    CHECK(!fires[0].on);
}

TEST(clip_player, fixed_snapshot_cap) {
    Clip big;
    big.length_ticks = 96 * 4;
    for (int i = 0; i < static_cast<int>(ClipPlayer::kMaxEvents) + 64; ++i) {
        big.insert_sorted(ClipEvent{i % (96 * 4), 0,
                                    static_cast<std::uint8_t>(i % 128),
                                    100, (i % 2) == 0, 0});
    }
    ClipPlayer p;
    p.publish(big, 120.0, 48000.0);
    // Bounded snapshot: the RT array never grows beyond kMaxEvents.
    CHECK_EQ(p.published_count(), ClipPlayer::kMaxEvents);
}

// --- loop machine end-to-end (real queues, real callback) ------------------

TEST(clip_playback, recorded_pad_is_audible_on_playback) {
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(-1, err));  // Drums → slot 0 (the engine starts EMPTY)
    eng.command({Action::TransportRecord, 0, 0});
    // Roll ~1 s so the take lands mid-loop, not crushed at tick 0.
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(9, 36, 100, true);  // pad A1 (channel 10)
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(9, 36, 0, false);
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(0));  // pads record into the Drums clip
    CHECK(!eng.clip_has_notes(1));

    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    eng.command({Action::TransportPlay, 0, 0});
    // One 4-bar loop: the recorded hit must be audible through the mix.
    float const peak = eng.test_render_audio(512, 800);
    CHECK(peak > 0.05f);
}

TEST(clip_playback, keys_record_in_any_octave) {
    // THE regression ("I can't record the instruments at all"): C5 is note
    // 72 — the old gate recorded only 36..43, so normal playing captured
    // nothing and playback stayed metronome-only.
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));  // Default Saw → slot 0 (starts EMPTY)
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 72, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 72, 0, false);
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(0));  // keys record into the SELECTED track

    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    eng.command({Action::TransportPlay, 0, 0});
    float const peak = eng.test_render_audio(512, 800);
    CHECK(peak > 0.02f);
}

TEST(clip_playback, stop_releases_and_quiets) {
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));  // Default Saw → slot 0
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 110, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 190);  // ~2 s held
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();

    eng.command({Action::TransportPlay, 0, 0});
    CHECK(eng.test_render_audio(512, 800) > 0.02f);  // audible…
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    eng.audio().transport_control().metronome_on.store(false);
    // …and after the stop edge (release tails) plus a full loop of decay,
    // silence — no hung voices.
    float const tail = eng.test_render_audio(512, 900);
    CHECK(tail < 0.01f);
}

TEST(clip_playback, first_note_starts_the_transport) {
    // "Recording should be done by user playing": Record ARMS (transport
    // stays stopped); the first played note starts the transport AND is
    // captured — the take begins with the note that triggered it.
    PlayEngine eng;
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportRecord, 0, 0});
    CHECK(eng.recording());
    // Transport still stopped: play_state mirror is 0.
    CHECK_EQ(eng.audio().transport_control().play_state.load(), 0);
    eng.test_feed_note(0, 60, 100, true);
    (void)eng.drain_app_events();
    // The note-on requested play — applied at the next buffer.
    float const warm = eng.test_render_audio(512, 4);
    (void)warm;
    CHECK(eng.audio().transport_control().play_state.load() != 0);
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(0));
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    // Stop DISARMS: no ghost capture on the next note.
    CHECK(!eng.recording());
}

TEST(clip_playback, clip_placement_moves_playback) {
    // "Record a portion and drop it in the play bar": start_tick places
    // the clip on the timeline — silent before it, audible at it.
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportRecord, 0, 0});
    eng.test_feed_note(0, 60, 120, true);
    (void)eng.drain_app_events();
    // Hold across real rolled time — a zero-length take is an instant
    // note-off and rightly inaudible.
    (void)eng.test_render_audio(512, 190);
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    // Let the RT side APPLY the stop — the request plane is latest-wins
    // within one buffer, so a Stop immediately followed by Play would
    // otherwise never stop (and playback would continue, not restart).
    (void)eng.test_render_audio(512, 2);

    // Place the clip 4 bars out (the drag commit path).
    eng.set_clip_start(0, 4);
    CHECK_EQ(eng.clip_start_bar(0), 4);

    eng.command({Action::TransportPlay, 0, 0});
    (void)eng.test_render_audio(512, 2);  // apply the play (pos → playhead)
    // Bars 0..~3.7: silent (the clip is parked at bar 4).
    float const before = eng.test_render_audio(512, 700);
    CHECK(before < 0.02f);
    // Bars ~3.7..~11.7: the clip fires at bar 4 and holds ~2 s.
    float const during = eng.test_render_audio(512, 770);
    CHECK(during > 0.02f);
}

TEST(clip_playback, clear_clip_removes_notes_keeps_track) {
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportRecord, 0, 0});
    eng.test_feed_note(0, 60, 100, true);
    (void)eng.drain_app_events();
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(0));

    eng.clear_clip(0);  // right-click → Clear recording
    CHECK(!eng.clip_has_notes(0));
    CHECK_EQ(eng.track_count(), 1);  // the TRACK survives
}

TEST(clip_playback, tracks_add_and_remove) {
    PlayEngine eng;
    eng.test_init_audio();
    // "It's not necessary to have keys a keys b drum and bass — I will add
    // them manually as needed": start empty, add from the menu, remove.
    std::string err;
    CHECK_EQ(eng.track_count(), 0);  // starts EMPTY
    CHECK(eng.add_track(0, err));    // Default Saw
    CHECK(eng.add_track(-1, err));   // Drum Kit
    CHECK_EQ(eng.track_count(), 2);
    CHECK(eng.drums_present());
    CHECK(eng.add_track(3, err));    // Pluck Keys
    CHECK_EQ(eng.track_count(), 3);
    CHECK(std::string(eng.track_name(0)) == "Default Saw");
    CHECK(eng.track_kind(1) == TrackKind::Drums);
    // Second drums track refused (shared kit).
    CHECK(!eng.add_track(-1, err));

    // Record a pad hit into the drums slot, then remove the SYNTH slot in
    // front of it — the drums clip must shift down with its track.
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.select_track(2);
    eng.test_feed_note(0, 60, 100, true);  // keys on the Pluck track
    (void)eng.drain_app_events();
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(2));

    CHECK(eng.remove_track(0, err));  // remove Default Saw
    CHECK_EQ(eng.track_count(), 2);
    CHECK(std::string(eng.track_name(0)) == "Drums");
    CHECK(std::string(eng.track_name(1)) == "Pluck Keys");
    CHECK(eng.clip_has_notes(1));  // the take moved with its track
    CHECK(!eng.clip_has_notes(0));
}

TEST(clip_playback, two_instruments_interleaved_keeps_both_takes) {
    // User report: "when i record an instrument it removes the old recorded
    // box — [rec 1] [rec 2] -> it removes rec 1, we want both, many
    // instruments can interleave each other recording". rec 1 on
    // instrument 1, rec 2 on instrument 2: BOTH clips keep their notes,
    // each drawing its own take box.
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));  // instrument 1 (auto-selected → slot 0)
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(0));  // rec 1
    CHECK_EQ(eng.clip_boxes(0).size(), 1u);

    CHECK(eng.add_track(3, err));  // instrument 2 — add_track selects it
    CHECK_EQ(eng.selected_track(), 1);
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 72, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 72, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    CHECK(eng.clip_has_notes(1));          // rec 2
    CHECK_EQ(eng.clip_boxes(1).size(), 1u);
    CHECK(eng.clip_has_notes(0));          // rec 1 SURVIVES
    CHECK_EQ(eng.clip_boxes(0).size(), 1u);
    CHECK(eng.clip_boxes(0)[0].has_notes);
    CHECK(eng.clip_boxes(1)[0].has_notes);
}

TEST(clip_playback, second_take_same_track_keeps_first_take) {
    // The punch-in variant of the same report: rec 1 finished, then rec 2
    // armed again on the SAME track at a later playhead position. The
    // take must ADD to the clip, not repossess its placement (the old
    // take's notes must stay playable where they were recorded).
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    auto const first_start = eng.clip(0).start_tick;
    auto const first_len = eng.clip(0).length_ticks;
    CHECK(eng.clip_has_notes(0));

    // Punch-in again at the same spot (transport restarted from the same
    // playhead): the take lands inside the existing take's box (merged —
    // no second block), rec 1 stays.
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 67, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 67, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    CHECK_EQ(eng.clip(0).start_tick, first_start);
    CHECK(eng.clip(0).length_ticks >= first_len);
    CHECK(eng.clip_has_notes(0));  // rec 1 + rec 2 both in the clip
    CHECK_EQ(eng.clip_boxes(0).size(), 1u);  // merged: one box, both takes
}

TEST(clip_playback, punch_in_at_later_position_keeps_first_box) {
    // rec 1 at the start, transport rolls on, punch-in again on the SAME
    // track at a later playhead position. TWO boxes — [rec 1] [rec 2] —
    // and BOTH audible where they were recorded. (Regression for the
    // repossession: take_cover used to MOVE the clip to the new take,
    // erasing rec 1's block from the canvas.)
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();
    auto const first_start = eng.clip(0).start_tick;
    CHECK(eng.clip_has_notes(0));

    // Move the playhead 8 bars out and punch in there.
    eng.set_playhead_bar(8);
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 4);  // apply the position request
    eng.test_feed_note(0, 67, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 67, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();

    // [rec 1] [rec 2]: two boxes, rec 1's still at its own position…
    auto const& boxes = eng.clip_boxes(0);
    CHECK_EQ(boxes.size(), 2u);
    CHECK_EQ(boxes[0].take, 0);
    CHECK_EQ(eng.clip(0).start_tick, first_start);
    CHECK_EQ(boxes[1].take, 1);
    CHECK_EQ(boxes[1].start_bar, 8);       // rec 2 landed where it was played
    CHECK(boxes[0].has_notes);
    CHECK(boxes[1].has_notes);
    // …and both takes' notes are in the track's clip.
    bool has_60 = false, has_67 = false;
    for (auto const& e : eng.clip(0).events) {
        if (e.note == 60) has_60 = true;
        if (e.note == 67) has_67 = true;
    }
    CHECK(has_60);
    CHECK(has_67);

    // Playback: the union window loops BOTH takes at their own spots.
    eng.command({Action::TransportPlay, 0, 0});
    float const peak = eng.test_render_audio(512, 900);
    CHECK(peak > 0.02f);
}

TEST(clip_playback, armed_roll_without_notes_leaves_no_ghost_box) {
    // Arming while rolling places the take's box before the first note;
    // a take that never played must not leave a block behind (the
    // stale-ghost bug class the user hit after removing instruments).
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportPlay, 0, 0});
    (void)eng.test_render_audio(512, 4);
    eng.command({Action::TransportRecord, 0, 0});  // punch-in arm, rolling
    (void)eng.drain_app_events();                  // covers → provisional box
    (void)eng.test_render_audio(512, 94);
    CHECK_EQ(eng.clip_boxes(0).size(), 1u);  // live REC box while armed
    eng.command({Action::TransportRecord, 0, 0});  // punch-out, no notes
    (void)eng.drain_app_events();
    CHECK(eng.clip_boxes(0).empty());        // pruned — no ghost
    CHECK(!eng.clip_has_notes(0));
}

TEST(clip_playback, punch_before_a_moved_window_keeps_every_note) {
    // Take 1 recorded, the take-group dragged to bar 4, then a new take
    // punched at bar 0 — LEFT of the window. The window extends leftward:
    // the old take's notes must keep their absolute spot (their locals
    // shift with the window) and stay INSIDE the loop window (a shorter
    // window would silently drop them from playback).
    PlayEngine eng;
    eng.test_init_audio();
    std::string err;
    CHECK(eng.add_track(0, err));
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 60, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();

    // Drag the whole group to bar 4 (the take-group move commit).
    eng.set_clip_start(0, 4);
    CHECK_EQ(eng.clip_start_bar(0), 4);
    CHECK_EQ(eng.clip(0).events[0].tick, 0);  // locals ride the window

    // Punch-in at bar 0.
    eng.set_playhead_bar(0);
    eng.command({Action::TransportRecord, 0, 0});
    (void)eng.test_render_audio(512, 4);  // apply the position request
    eng.test_feed_note(0, 67, 100, true);
    (void)eng.drain_app_events();
    (void)eng.test_render_audio(512, 94);
    eng.test_feed_note(0, 67, 0, false);
    (void)eng.drain_app_events();
    eng.command({Action::TransportStop, 0, 0});
    (void)eng.drain_app_events();

    CHECK_EQ(eng.clip(0).start_tick, 0);
    // The dragged take's notes kept their absolute spot (bar 4+) and sit
    // inside the window.
    bool note_60_at_bar_4 = false;
    for (auto const& e : eng.clip(0).events) {
        if (e.note == 60 && e.on && e.tick >= 4 * 96 * 4) {
            note_60_at_bar_4 = true;
            CHECK(eng.clip(0).length_ticks > e.tick);
        }
    }
    CHECK(note_60_at_bar_4);
    // [rec 2] [rec 1]: two boxes — the new take at bar 0, the dragged
    // take's box moved with the drag to bar 4.
    auto const& boxes = eng.clip_boxes(0);
    CHECK_EQ(boxes.size(), 2u);
    bool box_at_0 = false, box_at_4 = false;
    for (auto const& b : boxes) {
        if (b.start_bar == 0) box_at_0 = true;
        if (b.start_bar == 4) box_at_4 = true;
    }
    CHECK(box_at_0);
    CHECK(box_at_4);
}

TEST_MAIN()
