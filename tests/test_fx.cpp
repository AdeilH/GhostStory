// ghost_story — tests/test_fx.cpp
//
// The new FX blocks (v0.2 chain): compressor, bitcrusher, drive shapes,
// flanger, tremolo, reverb — plus the bypass/determinism/stability gates
// every block must hold. Style mirrors test_render.cpp (fx_render helpers,
// RMS/peak assertions, bit-exact bypass checks).
#include "test_framework.hpp"

#include "audio/fx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using ghost_story::audio::DriveShape;
using ghost_story::audio::FxChain;
using ghost_story::audio::FxParams;
using ghost_story::audio::Reverb;
using ghost_story::audio::ReverbParams;

namespace {

constexpr double kSr = 48000.0;
constexpr std::uint32_t kBlock = 512;

void fx_render(FxChain& fx, std::vector<float>& l, std::vector<float>& r) {
    for (std::size_t off = 0; off < l.size(); off += kBlock) {
        std::size_t const n =
            (l.size() - off < kBlock) ? l.size() - off : kBlock;
        fx.process(l.data() + off, r.data() + off,
                   static_cast<std::uint32_t>(n));
    }
}

// set_sample_rate -> set_tempo -> set_params (the established order).
FxChain make_fx(FxParams const& p) {
    FxChain fx;
    fx.set_sample_rate(kSr);
    fx.set_tempo(120.0);
    fx.set_params(p);
    return fx;
}

std::vector<float> sine(double hz, double sec, float amp) {
    std::vector<float> v(static_cast<std::size_t>(sec * kSr), 0.f);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = amp * static_cast<float>(
                          std::sin(2.0 * 3.14159265358979323846 * hz *
                                   static_cast<double>(i) / kSr));
    }
    return v;
}

std::vector<float> noise_like(std::uint64_t seed, double sec, float amp) {
    // Deterministic LCG white noise (matches the project's no-rand() rule).
    std::vector<float> v(static_cast<std::size_t>(sec * kSr), 0.f);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < v.size(); ++i) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        double const u =
            static_cast<double>(s >> 40) / 16777216.0;  // 0..1, 24 bits
        v[i] = amp * static_cast<float>(2.0 * u - 1.0);
    }
    return v;
}

double rms(std::vector<float> const& v, std::size_t from = 0) {
    double sum = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) {
        sum += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return std::sqrt(sum / static_cast<double>(v.size() - from));
}

float peak_abs(std::vector<float> const& v) {
    float p = 0.f;
    for (float x : v) {
        p = std::max(p, std::abs(x));
    }
    return p;
}

bool all_finite(std::vector<float> const& v) {
    for (float x : v) {
        if (!std::isfinite(x)) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- bypass gates ---------------------------------------------------------

TEST(fx, default_params_are_bit_exact_bypass) {
    // Every new block defaults off; a default FxParams must leave the
    // signal untouched sample-for-sample (the offline export determinism
    // contract depends on this).
    auto l0 = sine(440.0, 1.0, 0.5f);
    auto r0 = sine(554.37, 1.0, 0.4f);
    auto l = l0;
    auto r = r0;
    FxChain fx = make_fx(FxParams{});
    fx_render(fx, l, r);
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (l[i] != l0[i] || r[i] != r0[i]) {
            ghost_story::test::report_failure(__FILE__, __LINE__,
                                         "bypass altered samples");
            return;
        }
    }
}

// --- reverb ---------------------------------------------------------------

TEST(fx, reverb_adds_tail_to_impulse) {
    // A single-sample impulse: with reverb ON there must be audible energy
    // long after the impulse (a tail); with reverb OFF (mix 0) nothing.
    Reverb rv;
    rv.set_sample_rate(kSr);
    ReverbParams p;
    p.on = true;
    p.mix = 0.8f;
    p.decay = 0.8f;
    rv.set_params(p);
    std::vector<float> l(48000, 0.f);
    std::vector<float> r(48000, 0.f);
    l[0] = 1.0f;
    for (std::uint32_t off = 0; off < l.size(); off += kBlock) {
        std::uint32_t const n = static_cast<std::uint32_t>(
            std::min<std::size_t>(kBlock, l.size() - off));
        rv.process(l.data() + off, r.data() + off, n);
    }
    double const tail = rms(l, 9600);  // 200 ms after the impulse
    CHECK(tail > 1.0e-4);
    CHECK(all_finite(l));
    CHECK(peak_abs(l) < 4.0f);  // bounded (comb sum, not runaway)
}

TEST(fx, reverb_higher_decay_rings_longer) {
    auto make_tail = [&](float decay) {
        Reverb rv;
        rv.set_sample_rate(kSr);
        ReverbParams p;
        p.on = true;
        p.mix = 0.9f;
        p.decay = decay;
        rv.set_params(p);
        std::vector<float> l(48000, 0.f);
        std::vector<float> r(48000, 0.f);
        l[0] = 1.0f;
        for (std::uint32_t off = 0; off < l.size(); off += kBlock) {
            std::uint32_t const n = static_cast<std::uint32_t>(
                std::min<std::size_t>(kBlock, l.size() - off));
            rv.process(l.data() + off, r.data() + off, n);
        }
        return rms(l, 24000);  // second half = late tail
    };
    double const short_tail = make_tail(0.1f);
    double const long_tail = make_tail(0.95f);
    CHECK(long_tail > short_tail * 1.5);
}

TEST(fx, reverb_mix_zero_is_identity_even_when_on) {
    // The insert convention is out = dry + mix*wet: mix 0 must be exact.
    auto l0 = sine(220.0, 0.5, 0.3f);
    auto r0 = sine(330.0, 0.5, 0.3f);
    auto l = l0;
    auto r = r0;
    FxChain fx = make_fx([] {
        FxParams p;
        p.reverb_on = true;
        p.reverb_mix = 0.0f;
        p.reverb_decay = 0.9f;
        return p;
    }());
    fx_render(fx, l, r);
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (l[i] != l0[i]) {
            ghost_story::test::report_failure(
                __FILE__, __LINE__, "reverb mix=0 altered dry signal");
            return;
        }
    }
}

// --- compressor -----------------------------------------------------------

TEST(fx, compressor_tames_peaks) {
    // A hot sine above the threshold must come out quieter (gain < 1 above
    // threshold, makeup 1). Finite, bounded.
    auto l = sine(110.0, 1.0, 0.9f);
    auto r = l;
    FxChain fx = make_fx([] {
        FxParams p;
        p.comp_on = true;
        p.comp_threshold = 0.20f;
        p.comp_ratio = 4.0f;
        p.comp_makeup = 1.0f;
        return p;
    }());
    fx_render(fx, l, r);
    float const p_out = peak_abs(l);
    CHECK(p_out < 0.85f);        // clearly compressed
    CHECK(p_out > 0.05f);        // not squashed to silence
    CHECK(all_finite(l));
}

TEST(fx, compressor_bypass_when_off) {
    auto l0 = sine(110.0, 0.5, 0.9f);
    auto l = l0;
    auto r = l0;
    FxChain fx = make_fx(FxParams{});  // comp_on defaults false
    fx_render(fx, l, r);
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (l[i] != l0[i]) {
            ghost_story::test::report_failure(__FILE__, __LINE__,
                                         "comp off altered signal");
            return;
        }
    }
}

// --- bitcrusher -----------------------------------------------------------

TEST(fx, crusher_quantizes_and_holds) {
    auto l = noise_like(42, 0.5, 0.9f);
    auto r = l;
    FxChain fx = make_fx([] {
        FxParams p;
        p.crush_on = true;
        p.crush_bits = 3.0f;         // steps of 1/4
        p.crush_downsample = 4.0f;   // hold 4 samples
        return p;
    }());
    fx_render(fx, l, r);
    // Quantized: every sample sits on a k/4 grid point (with clamping).
    for (float x : l) {
        float const scaled = x * 4.0f;
        if (std::abs(scaled - std::round(scaled)) > 1.0e-3f) {
            ghost_story::test::report_failure(__FILE__, __LINE__,
                                         "crusher output not quantized");
            return;
        }
    }
    // Sample-hold: runs of identical consecutive values of length >= 4
    // must exist (free-running noise would never repeat).
    int run = 1;
    int best = 1;
    for (std::size_t i = 1; i < l.size(); ++i) {
        run = l[i] == l[i - 1] ? run + 1 : 1;
        best = std::max(best, run);
    }
    CHECK(best >= 4);
}

// --- drive shapes ---------------------------------------------------------

TEST(fx, drive_shapes_differ_and_stay_bounded) {
    auto base = sine(220.0, 0.5, 0.7f);
    float peaks[3];
    double rmss[3];
    for (int s = 0; s < 3; ++s) {
        auto l = base;
        auto r = base;
        FxChain fx = make_fx([s] {
            FxParams p;
            p.drive = 4.0f;
            p.drive_shape = s == 0 ? DriveShape::Soft
                            : s == 1 ? DriveShape::Hard
                                     : DriveShape::Fold;
            return p;
        }());
        fx_render(fx, l, r);
        peaks[static_cast<std::size_t>(s)] = peak_abs(l);
        rmss[static_cast<std::size_t>(s)] = rms(l);
        CHECK(peaks[static_cast<std::size_t>(s)] <= 1.01f);
        CHECK(all_finite(l));
    }
    // The three characters are distinct signals.
    CHECK(std::abs(rmss[0] - rmss[1]) > 1.0e-3);
    CHECK(std::abs(rmss[1] - rmss[2]) > 1.0e-3);
    // Foldback inverts loud passages: its RMS drops far below tanh's.
    CHECK(rmss[2] < rmss[0] * 0.8);
}

// --- flanger --------------------------------------------------------------

TEST(fx, flanger_changes_signal_and_survives_feedback) {
    auto l0 = sine(880.0, 1.5, 0.5f);
    auto l = l0;
    auto r = l0;
    FxChain fx = make_fx([] {
        FxParams p;
        p.flanger_on = true;
        p.flanger_rate_hz = 0.5f;
        p.flanger_depth_ms = 2.0f;
        p.flanger_feedback = 0.9f;
        p.flanger_mix = 0.8f;
        return p;
    }());
    fx_render(fx, l, r);
    // Different from dry after the ramp-in window.
    float max_delta = 0.f;
    for (std::size_t i = 9600; i < l.size(); ++i) {
        max_delta = std::max(max_delta, std::abs(l[i] - l0[i]));
    }
    CHECK(max_delta > 1.0e-3f);
    // Feedback 0.9 must NOT run away.
    CHECK(peak_abs(l) < 8.0f);
    CHECK(all_finite(l));
}

// --- tremolo --------------------------------------------------------------

TEST(fx, tremolo_modulates_amplitude) {
    // 4 Hz tremolo over a 2 s sine: the smoothed amplitude envelope must
    // swing. Measure short-window RMS at LFO minima vs maxima.
    auto l = sine(500.0, 2.0, 0.5f);
    auto r = l;
    FxChain fx = make_fx([] {
        FxParams p;
        p.tremolo_on = true;
        p.tremolo_rate_hz = 4.0f;
        p.tremolo_depth = 1.0f;
        return p;
    }());
    fx_render(fx, l, r);
    auto window_rms = [&](double t0) {
        std::size_t const a = static_cast<std::size_t>(t0 * kSr);
        double sum = 0.0;
        for (std::size_t i = a; i < a + 480; ++i) {  // 10 ms: near-instant
            sum += static_cast<double>(l[i]) * static_cast<double>(l[i]);
        }
        return std::sqrt(sum / 480.0);
    };
    // Gain g = 1 - 0.5*(1 + sin(2pi*4*t)): minimal (0) when 4t = 0.25 + k
    // -> t = 0.0625 + 0.25k; maximal (1) when 4t = 0.75 + k
    // -> t = 0.1875 + 0.25k. Narrow windows sample the extremes purely.
    double lo = 1e9;
    double hi = 0.0;
    for (int k = 0; k < 6; ++k) {
        lo = std::min(lo, window_rms(0.0625 + 0.25 * k));
        hi = std::max(hi, window_rms(0.1875 + 0.25 * k));
    }
    CHECK(hi > lo * 3.0);  // deep modulation
    CHECK(all_finite(l));
}

// --- stability + determinism ----------------------------------------------

TEST(fx, everything_on_is_stable_no_nan) {
    // Worst case: every block on, feedback high, drive max, noise input,
    // 3 s — must stay finite and bounded (the RT contract: no blowups).
    auto l = noise_like(7, 3.0, 0.9f);
    auto r = noise_like(8, 3.0, 0.9f);
    FxChain fx = make_fx([] {
        FxParams p;
        p.eq_low_db = 12.f;
        p.eq_mid_db = -12.f;
        p.eq_high_db = 15.f;
        p.comp_on = true;
        p.comp_threshold = 0.1f;
        p.comp_ratio = 20.0f;
        p.comp_makeup = 4.0f;
        p.crush_on = true;
        p.crush_bits = 2.0f;
        p.crush_downsample = 16.0f;
        p.drive = 8.0f;
        p.drive_shape = DriveShape::Fold;
        p.chorus_on = true;
        p.flanger_on = true;
        p.flanger_feedback = 0.9f;
        p.delay_on = true;
        p.delay_feedback = 0.9f;
        p.delay_mix = 0.9f;
        p.tremolo_on = true;
        p.reverb_on = true;
        p.reverb_mix = 1.0f;
        p.reverb_decay = 1.0f;
        return p;
    }());
    fx_render(fx, l, r);
    CHECK(all_finite(l));
    CHECK(all_finite(r));
    CHECK(peak_abs(l) < 16.0f);
    CHECK(peak_abs(r) < 16.0f);
}

TEST(fx, same_params_render_bit_identical) {
    auto l0 = noise_like(99, 1.0, 0.6f);
    auto r0 = noise_like(100, 1.0, 0.6f);
    auto build = [] {
        FxParams p;
        p.chorus_on = true;
        p.flanger_on = true;
        p.delay_on = true;
        p.tremolo_on = true;
        p.reverb_on = true;
        p.reverb_decay = 0.7f;
        return p;
    };
    auto a = l0;
    auto ar = r0;
    FxChain fa = make_fx(build());
    fx_render(fa, a, ar);
    auto b = l0;
    auto br = r0;
    FxChain fb = make_fx(build());
    fx_render(fb, b, br);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i] || ar[i] != br[i]) {
            ghost_story::test::report_failure(
                __FILE__, __LINE__, "same params rendered differently");
            return;
        }
    }
}

TEST(fx, tempo_changes_delay_length_alloc_free) {
    // set_tempo is RT-safe pure math: calling it mid-stream must not
    // change ring capacity or crash, and the echo must move with the beat.
    auto imp = std::vector<float>(96000 * 2, 0.f);  // 2 s
    auto rr = imp;
    imp[0] = 1.0f;
    FxChain fx = make_fx([] {
        FxParams p;
        p.delay_on = true;
        p.delay_beats = 0.5f;
        p.delay_feedback = 0.0f;
        p.delay_mix = 1.0f;
        return p;
    }());
    // Render 0.25 s, then change tempo (120 -> 60 doubles the echo time),
    // keep rendering half a second MORE so the relocated second echo is
    // inside processed territory: echo #1 at 0.25 s, echo #2 at 0.5 s.
    std::uint32_t const quarter = 12000;  // 0.25 s at 48 k
    fx.process(imp.data(), rr.data(), quarter);
    fx.set_tempo(60.0);
    fx.process(imp.data() + quarter, rr.data() + quarter, quarter * 2);
    // The echo follows delay_len_: with the tempo changed BEFORE delay_w_
    // ever reached 12000, there must be NO echo at 0.25 s...
    float at_quarter = 0.f;
    for (std::uint32_t i = quarter - 64; i < quarter + 64; ++i) {
        at_quarter = std::max(at_quarter, std::abs(imp[i]));
    }
    CHECK(at_quarter < 0.1f);
    // ...and the relocated echo of the original impulse at 0.5 s.
    float at_half = 0.f;
    for (std::uint32_t i = quarter * 2 - 64; i < quarter * 2 + 64; ++i) {
        at_half = std::max(at_half, std::abs(imp[i]));
    }
    CHECK(at_half > 0.5f);
}

TEST_MAIN()
