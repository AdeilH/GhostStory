// addu94 — tests/test_music_tools.cpp
// Property + unit tests: velocity curves, scale-lock, quantizer grids.
#include "test_framework.hpp"

#include "music/quantize.hpp"
#include "music/scales.hpp"
#include "music/velocity.hpp"

#include <vector>

using namespace addu94::music;

TEST(velocity, off_passes_through_and_bounds_clamp) {
    for (auto c : {VelocityCurve::Linear, VelocityCurve::Log,
                   VelocityCurve::Exp, VelocityCurve::HardFloor}) {
        CHECK_EQ(apply_velocity_curve(c, 0), 0);      // note-off untouched
        CHECK(apply_velocity_curve(c, 1) >= 1);
        CHECK(apply_velocity_curve(c, 1) <= 127);
        CHECK(apply_velocity_curve(c, 200) <= 127);
        CHECK(apply_velocity_curve(c, 127) >= apply_velocity_curve(c, 2));
    }
}

TEST(velocity, log_curve_gives_soft_notes_more_than_linear) {
    // A soft strike (v=20) on mini keys should produce MORE out than linear
    // under Log (that's the point of the default for mini keys).
    CHECK(apply_velocity_curve(VelocityCurve::Log, 20) >
          apply_velocity_curve(VelocityCurve::Linear, 20));
    // Exp compresses softs below linear.
    CHECK(apply_velocity_curve(VelocityCurve::Exp, 20) <
          apply_velocity_curve(VelocityCurve::Linear, 20));
    // Hard floor lifts the weak tail exactly to the floor.
    CHECK_EQ(apply_velocity_curve(VelocityCurve::HardFloor, 10), 32);
    CHECK_EQ(apply_velocity_curve(VelocityCurve::HardFloor, 40), 40);
}

TEST(velocity, log_curve_monotonic_no_plateaus) {
    int last = 0;
    for (int v = 1; v <= 127; ++v) {
        int const out = apply_velocity_curve(VelocityCurve::Log, v);
        CHECK(out >= last);
        last = out;
    }
    CHECK_EQ(last, 127);
}

TEST(scales, chromatic_is_identity) {
    auto const& chrom = scale_by_name("chromatic");
    for (int n = 0; n < 128; ++n) {
        CHECK_EQ(snap_to_scale(chrom, 0, n), n);
    }
}

TEST(scales, major_snaps_wrong_notes_up_or_down) {
    auto const& maj = scale_by_name("major");
    // C major (root 0): F# (66) → nearest G (67).
    CHECK_EQ(snap_to_scale(maj, 0, 66), 67);
    // B (59) is in scale; C (60) in scale; E (64) in scale.
    CHECK_EQ(snap_to_scale(maj, 0, 59), 59);
    // Db (61) → C (60) (down 1 vs up 1 → tie → up wins per our rule).
    // tie → up: 61 is 1 from C(60) and 2 from D(62): → 60. Recheck rule:
    // d=1: note+1=62 (D, in scale) → returns 62 FIRST (up wins ties).
    CHECK_EQ(snap_to_scale(maj, 0, 61), 62);
}

TEST(scales, snap_preserves_octave_center) {
    auto const& penta = scale_by_name("pentatonic-minor");
    // A minor penta root 9: all snaps stay within ±6 semitones.
    for (int n = 0; n < 128; ++n) {
        int const s = snap_to_scale(penta, 9, n);
        CHECK(std::abs(s - n) <= 6);
    }
}

TEST(quantize, off_is_identity) {
    CHECK_EQ(quantize_tick(Grid::Off, 37, 100), 37);
    CHECK_EQ(quantize_tick(Grid::Off, 0, 100), 0);
}

TEST(quantize, sixteenth_grid_is_24_ticks) {
    CHECK_EQ(grid_ticks(Grid::Sixteenth), 24);
    CHECK_EQ(quantize_tick(Grid::Sixteenth, 25, 100), 24);   // nearer below
    CHECK_EQ(quantize_tick(Grid::Sixteenth, 36, 100), 48);   // tie → up
    CHECK_EQ(quantize_tick(Grid::Sixteenth, 12, 100), 24);   // tie → up
    CHECK_EQ(quantize_tick(Grid::Sixteenth, 13, 100), 24);
}

TEST(quantize, strength_50_partial_move) {
    // tick 30 → target 24 (delta -6) at 50% → 27.
    CHECK_EQ(quantize_tick(Grid::Sixteenth, 30, 50), 27);
    // tick 18 → target 24 (delta +6) at 50% → 21.
    CHECK_EQ(quantize_tick(Grid::Sixteenth, 18, 50), 21);
}

TEST(quantize, triplets) {
    CHECK_EQ(grid_ticks(Grid::EighthT), 32);
    CHECK_EQ(quantize_tick(Grid::EighthT, 33, 100), 32);
    CHECK_EQ(quantize_tick(Grid::EighthT, 31, 100), 32);
    CHECK_EQ(grid_ticks(Grid::SixteenthT), 16);
}

TEST(quantize, never_negative) {
    CHECK_EQ(quantize_tick(Grid::Sixteenth, -5, 100), 0);
}

TEST(quantize, property_snaps_within_half_period) {
    for (std::int64_t t = 0; t < 10'000; ++t) {
        auto const q = quantize_tick(Grid::Sixteenth, t, 100);
        CHECK(std::abs(q - t) <= 12);  // half of 24
    }
}

TEST_MAIN()
