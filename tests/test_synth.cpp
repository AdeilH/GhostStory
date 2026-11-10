// ghost_story — tests/test_synth.cpp
// In-project synth tests (the agent selftest stays /tmp-only).
#include "test_framework.hpp"

#include "audio/synth.hpp"

#include <cmath>
#include <vector>

using ghost_story::audio::Synth;
using ghost_story::audio::SynthParams;

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
    CHECK(s.active_voices() <= ghost_story::audio::kMaxVoices);
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

// --- v0.2 additions: mono-legato/glide, velocity->filter, LFO2 -----------

TEST(synth, mono_legato_keeps_one_voice_per_channel) {
    Synth s;
    s.set_sample_rate(48000);
    SynthParams p;
    p.mono_legato = true;
    s.set_params(p);
    s.note_on(0, 60, 1.0f);
    s.note_on(0, 64, 1.0f);
    s.note_on(0, 67, 1.0f);
    CHECK_EQ(s.active_voices(), 1);
    // A different channel gets its own voice.
    s.note_on(1, 72, 1.0f);
    CHECK_EQ(s.active_voices(), 2);
}

TEST(synth, glide_ramps_pitch_upward) {
    Synth s;
    s.set_sample_rate(48000);
    SynthParams p;
    p.mono_legato = true;
    p.glide_seconds = 0.20f;
    p.attack = 0.002f;
    p.release = 0.5f;
    s.set_params(p);
    s.note_on(0, 48, 1.0f);   // C3 ~130.8 Hz
    auto base = render_for(s, 0.2);
    CHECK(base.peak > 0.01f);
    s.note_on(0, 72, 1.0f);   // C5 ~523 Hz: glide target
    // Zero-crossing rate just after the retrigger vs near the end of the
    // glide: the pitch must RISE toward 523 Hz (rate roughly 4x).
    auto rate_at = [&](std::size_t from, std::size_t to) {
        int crossings = 0;
        for (std::size_t i = from + 1; i < to; ++i) {
            if ((base.left[i - 1] < 0.f) != (base.left[i] < 0.f)) {
                ++crossings;
            }
        }
        return crossings;
    };
    (void)rate_at;  // crossings measured on the NEXT render below
    std::vector<float> after(static_cast<std::size_t>(48000 * 0.6), 0.f);
    for (std::uint32_t off = 0; off < after.size(); off += 512) {
        std::uint32_t const n =
            static_cast<std::uint32_t>(
                std::min<std::size_t>(512, after.size() - off));
        s.render(after.data() + off, after.data() + off, n);
    }
    auto cross_rate = [&](std::size_t from, std::size_t to) {
        int crossings = 0;
        for (std::size_t i = from + 1; i < to; ++i) {
            if ((after[i - 1] < 0.f) != (after[i] < 0.f)) {
                ++crossings;
            }
        }
        return crossings;
    };
    int const early = cross_rate(0, 1440);  // first 30 ms: glide starting
                                            // near 131 Hz
    int const late = cross_rate(14400, 19200);  // 300-400 ms: ~523 Hz
                                                // (glide 99.9 % done at
                                                // glide_seconds = 200 ms)
    CHECK(early > 6);    // audible oscillation at the start
    CHECK(late > early * 2);  // pitch clearly rose with the glide
    for (float v : after) {
        CHECK(!std::isnan(v));
    }
}

TEST(synth, velocity_filter_opens_cutoff) {
    // velocity_filter must add brightness BEYOND the linear velocity gain:
    // the loud/quiet RMS ratio grows with the parameter on (and equals the
    // pure-gain ratio with it off).
    auto ratio_for = [&](float vel_filter) {
        Synth s;
        s.set_sample_rate(48000);
        SynthParams p;
        p.velocity_filter = vel_filter;
        p.cutoff_base_hz = 200.f;  // dark enough that the octave(s) of
                                   // filter travel dominate the RMS
        p.attack = 0.002f;
        s.set_params(p);
        s.note_on(0, 60, 0.1f);
        auto quiet = render_for(s, 0.3);
        s.note_off(0, 60);
        render_for(s, 0.5);  // let it release
        s.note_on(0, 60, 1.0f);
        auto loud = render_for(s, 0.3);
        auto rms = [&](Rendered const& r) {
            double sum = 0.0;
            for (float v : r.left) {
                sum += static_cast<double>(v) * static_cast<double>(v);
            }
            return std::sqrt(sum / static_cast<double>(r.left.size()));
        };
        double const rq = std::max(rms(quiet), 1.0e-9);
        return rms(loud) / rq;
    };
    double const off = ratio_for(0.0f);
    double const on = ratio_for(1.0f);
    CHECK(off > 1.0);         // sanity: loud is louder
    CHECK(on > off * 1.5);    // filter opening adds real brightness
}

TEST(synth, lfo2_amp_tremolo_changes_signal) {
    Synth s;
    s.set_sample_rate(48000);
    SynthParams base;
    base.attack = 0.002f;
    base.sustain = 1.0f;
    base.lfo2_target = ghost_story::audio::LfoTarget::Amp;
    base.lfo2_rate_hz = 6.0f;
    base.lfo2_depth = 0.9f;

    s.set_params(base);
    s.note_on(0, 69, 1.0f);
    auto trem = render_for(s, 0.5);
    s.all_sound_off();

    base.lfo2_target = ghost_story::audio::LfoTarget::Off;
    s.set_params(base);
    s.note_on(0, 69, 1.0f);
    auto dry = render_for(s, 0.5);

    // Deep 6 Hz tremolo must dip near silence at the LFO minima; the dry
    // render must not.
    float trem_min = 1e9f;
    for (std::size_t i = 9600; i < trem.left.size(); i += 96) {
        float a = 0.f;
        for (std::size_t j = i; j < i + 48 && j < trem.left.size(); ++j) {
            a = std::max(a, std::abs(trem.left[j]));
        }
        trem_min = std::min(trem_min, a);
    }
    float dry_min = 1e9f;
    for (std::size_t i = 9600; i < dry.left.size(); i += 96) {
        float a = 0.f;
        for (std::size_t j = i; j < i + 48 && j < dry.left.size(); ++j) {
            a = std::max(a, std::abs(dry.left[j]));
        }
        dry_min = std::min(dry_min, a);
    }
    CHECK(trem_min < dry_min * 0.2f);
    CHECK(dry_min > 0.05f);  // steady tone stays loud
}

TEST_MAIN()
