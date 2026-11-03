// addu94 — tests/test_synth.cpp
// In-project synth tests (the agent selftest stays /tmp-only).
#include "test_framework.hpp"

#include "audio/synth.hpp"

#include <cmath>
#include <vector>

using addu94::audio::Synth;
using addu94::audio::SynthParams;

namespace {
struct Rendered {
    std::vector<float> left, right;
    float peak{0.f};
};
Rendered render_for(Synth& s, double seconds) {
    Rendered r;
    std::uint32_t const frames = static_cast<std::uint32_t>(48000 * seconds);
    r.left.assign(frames, 0.f);
    r.right.assign(frames, 0.f);
    for (std::uint32_t off = 0; off < frames; off += 512) {
        std::uint32_t const n = (frames - off < 512) ? frames - off : 512;
        s.render(r.left.data() + off, r.right.data() + off, n);
    }
    for (float v : r.left) r.peak = std::max(r.peak, std::abs(v));
    return r;
}
}  // namespace

TEST(synth, renders_audible_tone) {
    Synth s;
    s.set_sample_rate(48000);
    s.note_on(0, 69, 1.0f);
    auto r = render_for(s, 0.5);
    CHECK(r.peak > 0.05f);
    CHECK(r.peak <= 1.0f);
    for (float v : r.left) {
        CHECK(!std::isnan(v));
        CHECK(!std::isinf(v));
    }
}

TEST(synth, note_off_releases_to_silence) {
    Synth s;
    s.set_sample_rate(48000);
    s.note_on(0, 60, 1.0f);
    auto r = render_for(s, 0.1);
    CHECK(r.peak > 0.01f);
    s.note_off(0, 60);
    auto r2 = render_for(s, 1.0);  // > default release 0.25 s
    CHECK_EQ(s.active_voices(), 0);
    // Measure only the tail (after release has fully elapsed).
    float tail_peak = 0.f;
    for (std::size_t i = r2.left.size() - 12000; i < r2.left.size(); ++i) {
        tail_peak = std::max(tail_peak, std::abs(r2.left[i]));
    }
    CHECK(tail_peak < 0.01f);
}

TEST(synth, all_sound_off_is_exact_zero_after) {
    Synth s;
    s.set_sample_rate(48000);
    for (int n = 40; n < 80; n += 3) s.note_on(0, n, 0.8f);
    auto r = render_for(s, 0.1);
    CHECK(r.peak > 0.01f);
    s.all_sound_off();
    std::vector<float> buf(4800, 1.f);
    s.render(buf.data(), buf.data(), 4800);
    for (float v : buf) CHECK_EQ(v, 0.f);
    CHECK_EQ(s.active_voices(), 0);
}

TEST(synth, voice_stealing_bounded_under_stress) {
    Synth s;
    s.set_sample_rate(48000);
    for (int i = 0; i < 200; ++i) s.note_on(0, 40 + (i % 24), 0.5f);
    CHECK(s.active_voices() <= addu94::audio::kMaxVoices);
    auto r = render_for(s, 0.2);
    for (float v : r.left) {
        CHECK(!std::isnan(v));
        CHECK(!std::isinf(v));
    }
}

TEST(synth, silent_until_note) {
    Synth s;
    s.set_sample_rate(48000);
    auto r = render_for(s, 0.05);
    CHECK_EQ(r.peak, 0.f);
    CHECK_EQ(s.active_voices(), 0);
}

TEST_MAIN()
