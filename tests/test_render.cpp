// addu94 — tests/test_render.cpp
//
// FX chain + offline renderer + WAV export tests (spec §2.7, M6 gates).
// Includes the M6 gate test named exactly "render.master_equals_sum_of_stems".
#include "test_framework.hpp"

#include "audio/fx.hpp"
#include "audio/render_engine.hpp"
#include "music/clip.hpp"
#include "project/export_wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

using addu94::audio::FxChain;
using addu94::audio::FxParams;
using addu94::audio::RenderRequest;
using addu94::audio::RenderResult;
using addu94::audio::SynthParams;
using addu94::music::Clip;
using addu94::music::ClipEvent;
using addu94::project::ExportSpec;

namespace {

constexpr double kSr = 48000.0;
constexpr std::uint32_t kBlock = 512;

// Render planar buffers through an FxChain in 512-frame blocks, in place.
void fx_render(FxChain& fx, std::vector<float>& l, std::vector<float>& r) {
    for (std::size_t off = 0; off < l.size(); off += kBlock) {
        std::size_t const n =
            (l.size() - off < kBlock) ? l.size() - off : kBlock;
        fx.process(l.data() + off, r.data() + off,
                   static_cast<std::uint32_t>(n));
    }
}

FxChain make_fx(FxParams const& p) {
    FxChain fx;
    fx.set_sample_rate(kSr);
    fx.set_tempo(120.0);
    fx.set_params(p);
    return fx;
}

double rms(std::vector<float> const& v) {
    if (v.empty()) {
        return 0.0;
    }
    double acc = 0.0;
    for (float x : v) {
        acc += static_cast<double>(x) * static_cast<double>(x);
    }
    return std::sqrt(acc / static_cast<double>(v.size()));
}

std::vector<float> sine(double hz, double seconds, float amp) {
    std::vector<float> v(static_cast<std::size_t>(kSr * seconds), 0.0f);
    for (std::size_t i = 0; i < v.size(); ++i) {
        double const t = static_cast<double>(i) / kSr;
        v[i] = static_cast<float>(static_cast<double>(amp) *
                                  std::sin(6.283185307179586 * hz * t));
    }
    return v;
}

Clip clip_with(std::vector<ClipEvent> evs, std::int64_t length_ticks) {
    Clip c;
    c.length_ticks = length_ticks;
    c.events = std::move(evs);
    return c;
}

RenderRequest make_request(Clip const* clip) {
    RenderRequest r;
    r.clip = clip;
    r.tempo_bpm = 120.0;
    r.sample_rate = kSr;
    return r;
}

struct WavData {
    std::vector<float> left, right;
};

// Minimal reader for the files our writer produces (walks RIFF chunks).
bool read_wav_f32(std::string const& path, WavData& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || bytes[0] != 'R' || bytes[1] != 'I' ||
        bytes[2] != 'F' || bytes[3] != 'F' || bytes[8] != 'W' ||
        bytes[9] != 'A' || bytes[10] != 'V' || bytes[11] != 'E') {
        return false;
    }
    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        std::uint32_t const sz =
            static_cast<std::uint32_t>(bytes[pos + 4]) |
            (static_cast<std::uint32_t>(bytes[pos + 5]) << 8) |
            (static_cast<std::uint32_t>(bytes[pos + 6]) << 16) |
            (static_cast<std::uint32_t>(bytes[pos + 7]) << 24);
        if (bytes[pos] == 'd' && bytes[pos + 1] == 'a' &&
            bytes[pos + 2] == 't' && bytes[pos + 3] == 'a') {
            std::size_t const frames = sz / 8u;
            out.left.resize(frames);
            out.right.resize(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                std::size_t const o = pos + 8 + i * 8;
                float l, r;
                std::memcpy(&l, bytes.data() + o, 4);
                std::memcpy(&r, bytes.data() + o + 4, 4);
                out.left[i] = l;
                out.right[i] = r;
            }
            return true;
        }
        pos += 8u + sz + (sz & 1u);
    }
    return false;
}

std::filesystem::path make_temp_dir() {
    std::filesystem::path p = std::filesystem::temp_directory_path() /
                              ("addu94_test_render_" +
                               std::to_string(static_cast<long>(::getpid())));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

}  // namespace

// ---------------------------------------------------------------- fx ---

TEST(fx, eq_low_shelf_boost_and_cut_bass) {
    auto const in = sine(100.0, 0.5, 0.5f);
    FxParams flat, boost, cut;
    boost.eq_low_db = 12.0f;
    cut.eq_low_db = -12.0f;
    auto run = [](FxParams const& p) {
        FxChain fx = make_fx(p);
        std::vector<float> l = sine(100.0, 0.5, 0.5f);
        std::vector<float> r = l;
        fx_render(fx, l, r);
        return rms(l);
    };
    double const flat_rms = run(flat);
    double const boost_rms = run(boost);
    double const cut_rms = run(cut);
    CHECK(flat_rms > 0.3);          // sanity: ~0.354 for 0.5 amplitude
    CHECK(boost_rms > 1.8 * flat_rms);  // +12 dB shelf: ~3x at 100 Hz
    CHECK(cut_rms < 0.65 * flat_rms);
}

TEST(fx, eq_high_shelf_boosts_treble) {
    auto run = [](float db) {
        FxParams p;
        p.eq_high_db = db;
        FxChain fx = make_fx(p);
        std::vector<float> l = sine(6000.0, 0.25, 0.5f);
        std::vector<float> r = l;
        fx_render(fx, l, r);
        return rms(l);
    };
    CHECK(run(12.0f) > 1.8 * run(0.0f));
}

TEST(fx, delay_echo_at_expected_offset) {
    // 1/16 * 4 = 0.25 beats at 120 bpm = 0.125 s = 6000 samples @ 48 kHz.
    FxParams p;
    p.delay_on = true;
    p.delay_beats = 0.25f;
    p.delay_feedback = 0.0f;
    p.delay_mix = 1.0f;
    FxChain fx = make_fx(p);
    std::size_t const frames = 30000;
    std::vector<float> l(frames, 0.0f);
    std::vector<float> r(frames, 0.0f);
    l[0] = 1.0f;
    fx_render(fx, l, r);

    std::vector<std::size_t> peaks;
    for (std::size_t i = 0; i < frames; ++i) {
        if (std::fabs(l[i]) > 0.3f) {
            peaks.push_back(i);
        }
    }
    CHECK_EQ(peaks.size(), std::size_t{2});
    if (peaks.size() == 2) {
        CHECK_EQ(peaks[0], std::size_t{0});
        double const expected = 0.25 * (60.0 / 120.0) * kSr;
        CHECK(std::fabs(static_cast<double>(peaks[1]) - expected) <= 32.0);
        CHECK(l[peaks[1]] > 0.5f);  // full wet mix
    }
}

TEST(fx, chorus_changes_signal) {
    FxParams off, on;
    on.chorus_on = true;
    auto run = [](FxParams const& p) {
        FxChain fx = make_fx(p);
        std::vector<float> l = sine(200.0, 0.5, 0.5f);
        std::vector<float> r = l;
        fx_render(fx, l, r);
        return l;
    };
    auto const dry = run(off);
    auto const wet = run(on);
    float max_diff = 0.0f;
    for (std::size_t i = 9600; i < dry.size(); ++i) {  // skip LFO/ring ramp-in
        max_diff = std::max(max_diff, std::fabs(dry[i] - wet[i]));
    }
    CHECK(max_diff > 1.0e-3f);
}

TEST(fx, soft_clip_drive_1_is_identity) {
    FxParams p;
    p.drive = 1.0f;
    FxChain fx = make_fx(p);
    std::vector<float> l = sine(300.0, 0.2, 1.5f);  // exceeds unity on purpose
    std::vector<float> r = sine(300.0, 0.2, -1.5f);
    auto const in_l = l;
    auto const in_r = r;
    fx_render(fx, l, r);
    float max_delta = 0.0f;
    for (std::size_t i = 0; i < l.size(); ++i) {
        max_delta = std::max(max_delta, std::fabs(l[i] - in_l[i]));
        max_delta = std::max(max_delta, std::fabs(r[i] - in_r[i]));
    }
    CHECK(max_delta < 1.0e-6f);
}

TEST(fx, soft_clip_limits_at_high_drive) {
    FxParams p;
    p.drive = 4.0f;
    FxChain fx = make_fx(p);
    std::vector<float> l = sine(100.0, 0.2, 3.0f);
    std::vector<float> r = l;
    fx_render(fx, l, r);
    float peak = 0.0f;
    for (std::size_t i = 0; i < l.size(); ++i) {
        peak = std::max(peak, std::fabs(l[i]));
        peak = std::max(peak, std::fabs(r[i]));
        CHECK(!std::isnan(l[i]));
        CHECK(!std::isnan(r[i]));
    }
    CHECK(peak <= 1.01f);
    CHECK(peak > 0.9f);  // still audible, not crushed to zero
}

TEST(fx, delay_ring_bounded_no_feedback_runaway) {
    FxParams p;
    p.delay_on = true;
    p.delay_beats = 0.5f;
    p.delay_feedback = 0.9f;
    p.delay_mix = 0.5f;
    FxChain fx = make_fx(p);
    std::vector<float> l = sine(220.0, 2.0, 0.8f);
    std::vector<float> r = l;
    fx_render(fx, l, r);
    for (float v : l) {
        CHECK(std::fabs(v) < 8.0f);  // feedback 0.9 must not run away
        CHECK(!std::isnan(v));
    }
}

// ------------------------------------------------------ render_track ---

TEST(render, deterministic_bit_identical) {
    Clip clip = clip_with(
        {
            ClipEvent{0, 0, 60, 127, true, 0},
            ClipEvent{48, 0, 64, 100, true, 0},
            ClipEvent{96, 0, 60, 0, false, 0},
            ClipEvent{192, 0, 64, 0, false, 0},
            ClipEvent{240, 0, 67, 110, true, 0},
            ClipEvent{383, 0, 67, 0, false, 0},
        },
        96 * 4);
    RenderRequest req = make_request(&clip);
    auto a = addu94::audio::render_track(req, 3);
    auto b = addu94::audio::render_track(req, 3);
    CHECK_EQ(a.left.size(), b.left.size());
    CHECK(a.left == b.left);
    CHECK(a.right == b.right);
}

TEST(render, silent_clip_is_silent) {
    Clip clip;  // no events
    clip.length_ticks = 96 * 4;
    RenderRequest req = make_request(&clip);
    auto out = addu94::audio::render_track(req, 2);
    CHECK_EQ(out.left.size(), out.right.size());
    // 96*4 ticks = 4 beats = 2 s = 96000 samples @ 48 kHz; x2 loops.
    CHECK_EQ(out.left.size(), std::size_t{192000});
    for (float v : out.left) {
        CHECK_EQ(v, 0.0f);
    }
    for (float v : out.right) {
        CHECK_EQ(v, 0.0f);
    }
}

TEST(render, note_at_tick_0_sounds) {
    Clip clip = clip_with(
        {
            ClipEvent{0, 0, 60, 127, true, 0},
            ClipEvent{96 * 2, 0, 60, 0, false, 0},
        },
        96 * 4);
    RenderRequest req = make_request(&clip);
    auto out = addu94::audio::render_track(req, 1);
    float peak = 0.0f;
    for (float v : out.left) {
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak > 0.05f);
    // Sound must start at sample 0 region (within the 10 ms attack).
    float early_peak = 0.0f;
    for (std::size_t i = 0; i < 480; ++i) {
        early_peak = std::max(early_peak, std::fabs(out.left[i]));
    }
    CHECK(early_peak > 0.02f);
}

TEST(render, no_clip_or_zero_loops_is_empty) {
    Clip clip;
    clip.length_ticks = 96;
    RenderRequest req = make_request(&clip);
    CHECK(addu94::audio::render_track(req, 0).left.empty());
    RenderRequest none = req;
    none.clip = nullptr;
    CHECK(addu94::audio::render_track(none, 1).left.empty());
}

TEST(render, loop_count_extends_length) {
    Clip clip = clip_with({ClipEvent{0, 0, 60, 127, true, 0},
                           ClipEvent{95, 0, 60, 0, false, 0}},
                          96);
    RenderRequest req = make_request(&clip);
    auto one = addu94::audio::render_track(req, 1);
    auto four = addu94::audio::render_track(req, 4);
    CHECK_EQ(one.left.size(), std::size_t{24000});   // 1 beat @ 120 bpm = 0.5 s
    CHECK_EQ(four.left.size(), std::size_t{96000});
    // Loops must repeat once steady state is reached: loop 1 starts from
    // silence, but the note's release tail bleeds across loop boundaries,
    // so from loop 2 on every loop starts in the same state. Compare
    // loop 2 vs loop 3 sample-exactly.
    CHECK(std::equal(four.left.begin() + static_cast<std::ptrdiff_t>(24000),
                     four.left.begin() + static_cast<std::ptrdiff_t>(48000),
                     four.left.begin() + static_cast<std::ptrdiff_t>(48000)));
}

// ------------------------------------------------------------ mixdown ---

// The M6 tolerance gate (spec: master == sum of stems within 1e-6).
TEST(render, master_equals_sum_of_stems) {
    Clip a = clip_with({ClipEvent{0, 0, 48, 127, true, 0},
                        ClipEvent{96 * 2, 0, 48, 0, false, 0}},
                       96 * 4);
    Clip b = clip_with({ClipEvent{96, 0, 72, 127, true, 0},
                        ClipEvent{96 * 3, 0, 72, 0, false, 0}},
                       96 * 4);
    RenderRequest ra = make_request(&a);
    RenderRequest rb = make_request(&b);
    std::vector<RenderResult> stems;
    stems.push_back(addu94::audio::render_track(ra, 1));
    stems.push_back(addu94::audio::render_track(rb, 1));
    auto master = addu94::audio::mix_down(stems);
    CHECK_EQ(master.left.size(), stems[0].left.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < master.left.size(); ++i) {
        float const sum = stems[0].left[i] + stems[1].left[i];
        worst = std::max(worst,
                         static_cast<double>(std::fabs(master.left[i] - sum)));
        CHECK(std::fabs(static_cast<double>(master.right[i] -
                                            (stems[0].right[i] +
                                             stems[1].right[i]))) < 1.0e-6);
    }
    CHECK(worst < 1.0e-6);
    // Non-trivial audio actually flowed through the gate.
    float mpeak = 0.0f;
    for (float v : master.left) {
        mpeak = std::max(mpeak, std::fabs(v));
    }
    CHECK(mpeak > 0.05f);
}

// ------------------------------------------------------------- export ---

TEST(export, writes_stems_and_master) {
    auto dir = make_temp_dir();
    Clip a = clip_with({ClipEvent{0, 0, 45, 127, true, 0},
                        ClipEvent{96 * 2, 0, 45, 0, false, 0}},
                       96 * 4);
    Clip b = clip_with({ClipEvent{96, 0, 76, 127, true, 0},
                        ClipEvent{96 * 3, 0, 76, 0, false, 0}},
                       96 * 4);
    ExportSpec spec;
    spec.output_dir = dir.string();
    spec.tracks.push_back(make_request(&a));
    spec.tracks.push_back(make_request(&b));
    spec.loop_count = 1;

    std::string error;
    bool const ok = addu94::project::export_wav_stems(spec, error);
    CHECK(ok);
    if (!ok) {
        std::fprintf(stderr, "  export error: %s\n", error.c_str());
        return;
    }
    CHECK(error.empty());

    std::size_t const frames = static_cast<std::size_t>(96000);  // 4 beats @ 48k
    WavData s1, s2, master;
    CHECK(read_wav_f32((dir / "track_1.wav").string(), s1));
    CHECK(read_wav_f32((dir / "track_2.wav").string(), s2));
    CHECK(read_wav_f32((dir / "master.wav").string(), master));
    CHECK_EQ(s1.left.size(), frames);
    CHECK_EQ(master.left.size(), frames);

    // M6 gate on the written files: master == sum of stems (1e-6).
    double worst = 0.0;
    for (std::size_t i = 0; i < master.left.size(); ++i) {
        worst = std::max(
            worst, static_cast<double>(
                       std::fabs(master.left[i] - (s1.left[i] + s2.left[i]))));
    }
    CHECK(worst < 1.0e-6);

    // Sum of two uncorrelated stems: master RMS >= max stem RMS - epsilon.
    double const r1 = rms(s1.left);
    double const r2 = rms(s2.left);
    double const rm = rms(master.left);
    CHECK(r1 > 0.01);
    CHECK(r2 > 0.01);
    CHECK(rm >= std::max(r1, r2) - 1.0e-4);

    // Header sanity: sizes must be 44 + frames * 8 bytes.
    for (char const* name : {"track_1.wav", "track_2.wav", "master.wav"}) {
        auto const sz = std::filesystem::file_size(dir / name);
        CHECK_EQ(sz, std::uintmax_t{44} + static_cast<std::uintmax_t>(frames) * 8u);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(export, empty_spec_fails_with_error) {
    ExportSpec spec;
    spec.output_dir = "/tmp/addu94_test_render_empty_spec";
    std::string error;
    CHECK(!addu94::project::export_wav_stems(spec, error));
    CHECK(!error.empty());
}

TEST_MAIN()
