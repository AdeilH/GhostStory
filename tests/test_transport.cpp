// ghost_story — tests/test_transport.cpp
// Property tests for clock math (spec §11): tempo changes, loop wrap,
// metronome scheduling, and bit-exact metronome determinism.
#include "test_framework.hpp"

#include "music/metronome.hpp"
#include "music/transport.hpp"

#include <cmath>
#include <vector>

using ghost_story::music::kPPQ;
using ghost_story::music::PlayState;
using ghost_story::music::Transport;
using ghost_story::music::TransportControl;

namespace {
struct Rig {
    TransportControl ctrl;
    Transport t{ctrl};
    Rig() { t.set_sample_rate(48000); }
};
}  // namespace

TEST(transport, position_monotonic_across_tempo_changes) {
    Rig r;
    r.t.start();
    std::uint64_t last = 0;
    double bpm = 20.0;
    for (int i = 0; i < 1000; ++i) {
        r.ctrl.tempo_bpm.store(bpm);
        r.t.advance(128);
        CHECK(r.t.position() >= last);
        last = r.t.position();
        bpm += 0.313;  // sweep 20 → 333 across the run
    }
    // No wrap happened (no loop set): position = 128000 exactly.
    CHECK_EQ(r.t.position(), 128000u);
}

TEST(transport, beats_match_samples_exactly_at_constant_tempo) {
    Rig r;
    r.ctrl.tempo_bpm.store(120.0);  // 0.5 s/beat → 24000 samples/beat @48k
    r.t.start();
    r.t.advance(24000);
    CHECK_NEAR(r.t.beats(), 1.0, 1e-9);
    r.t.advance(24000 * 3);  // +1 bar (4/4)
    CHECK_NEAR(r.t.beats(), 4.0, 1e-9);
    CHECK_NEAR(r.t.bars(), 1.0, 1e-9);
}

TEST(transport, ticks_are_96_per_quarter) {
    Rig r;
    r.ctrl.tempo_bpm.store(90.0);
    r.t.start();
    // 90 bpm → 2/3 s per beat → 32000 samples per beat.
    r.t.advance(32000);
    CHECK_NEAR(r.t.ticks(), double{kPPQ}, 1e-9);
}

TEST(transport, loop_wraps_sample_exactly) {
    Rig r;
    r.ctrl.tempo_bpm.store(120.0);
    r.t.set_loop_bars(2);  // 2 bars of 4/4 @120 = 8 beats = 192000 samples
    r.t.start();
    for (int i = 0; i < 1000; ++i) r.t.advance(512);
    // 512000 total → 512000 % 192000 = 128000 → 1.333… bars.
    CHECK_EQ(r.t.position(), 128000u);
    CHECK_NEAR(r.t.bars(), 4.0 / 3.0, 1e-9);
}

TEST(transport, stop_freezes_and_start_resets) {
    Rig r;
    r.t.start();
    r.t.advance(1000);
    r.t.stop();
    r.t.advance(1000);
    CHECK_EQ(r.t.position(), 1000u);
    r.t.start();
    CHECK_EQ(r.t.position(), 0u);
}

TEST(transport, metronome_clicks_land_on_beats_4_4) {
    Rig r;
    r.ctrl.tempo_bpm.store(120.0);  // 24000 samples per beat
    r.ctrl.metronome_on.store(true);
    r.t.start();
    // Advance 2 beats in 4 buffers of 12000: clicks at 0 and 24000.
    Transport::Click clicks[8];
    int const n1 = r.t.clicks_in(12000, clicks, 8);
    CHECK_EQ(n1, 1);
    CHECK_EQ(clicks[0].offset, 0u);
    CHECK(clicks[0].accent);  // beat 0 = bar start
    r.t.advance(12000);
    int const n2 = r.t.clicks_in(12000, clicks, 8);
    CHECK_EQ(n2, 0);  // beat 1 lands at 24000, outside [12000, 24000)
    r.t.advance(12000);
    int const n3 = r.t.clicks_in(12000, clicks, 8);
    CHECK_EQ(n3, 1);
    CHECK_EQ(clicks[0].offset, 0u);
    CHECK(!clicks[0].accent);  // beat 2 of the bar
}

TEST(transport, metronome_follows_tempo_change) {
    Rig r;
    r.ctrl.metronome_on.store(true);
    r.ctrl.tempo_bpm.store(60.0);  // 1 beat/s → 48000 samples
    r.t.start();
    Transport::Click clicks[8];
    CHECK_EQ(r.t.clicks_in(48000, clicks, 8), 1);
    r.ctrl.tempo_bpm.store(120.0);  // now 24000
    r.t.advance(48000);             // latch happens in advance
    CHECK_EQ(r.t.clicks_in(48000, clicks, 8), 2);
    CHECK_EQ(clicks[0].offset, 0u);
    CHECK_EQ(clicks[1].offset, 24000u);
}

TEST(transport, tempo_sweep_beats_track_samples_property) {
    // Property: at any instant, beats() = position / (sr * 60 / bpm * ...)
    // within floating tolerance, even after arbitrary tempo jumps.
    Rig r;
    r.t.start();
    unsigned seed = 12345;
    for (int i = 0; i < 500; ++i) {
        seed = seed * 1103515245u + 12345u;
        double const bpm = 40.0 + static_cast<double>(seed % 20000) / 100.0;
        r.ctrl.tempo_bpm.store(bpm);
        r.t.advance(333);
        double const spb = 60.0 / bpm;
        double const expect_beats =
            static_cast<double>(r.t.position()) / (48000.0 * spb);
        CHECK_NEAR(r.t.beats(), expect_beats, 1e-6);
    }
}

TEST(metronome, bit_identical_for_identical_inputs) {
    std::vector<float> a(48000, 0.f), b(48000, 0.f);
    ghost_story::music::render_click(a.data(), a.size(), 100, true, 0.5f, 48000.0);
    ghost_story::music::render_click(b.data(), b.size(), 100, true, 0.5f, 48000.0);
    CHECK(a == b);
    // And different accents differ.
    std::vector<float> c(48000, 0.f);
    ghost_story::music::render_click(c.data(), c.size(), 100, false, 0.5f, 48000.0);
    CHECK(a != c);
}

TEST(metronome, click_is_bounded_and_decays) {
    std::vector<float> buf(4800, 0.f);
    ghost_story::music::render_click(buf.data(), buf.size(), 0, true, 1.0f, 48000.0);
    float peak = 0.f;
    for (float v : buf) {
        CHECK(std::abs(v) <= 1.0f);
        peak = std::max(peak, std::abs(v));
    }
    CHECK(peak > 0.5f);   // audible
    CHECK(std::abs(buf[2000]) < 0.01f);  // decayed to silence
}

TEST_MAIN()
