// addu94 — src/audio/drum_kit.cpp
//
// Fallback-kit synthesis implementing drum_kit.hpp. Control-thread only.
//
// Deterministic by construction: noise comes from a fixed-seed 64-bit LCG
// (never rand()), oscillators are plain phase accumulators, envelopes are
// exponentials. Two inits at the same rate produce bit-identical kits
// (tested). Each sound is normalized to its own distinct peak level so all
// eight pads stay distinguishable after the Sampler's master soft clip.
//
// Sound recipes (all mono, 0.05..1.4 s):
//   kick  — sine sweep 154 -> 44 Hz (tau 60 ms) + 5 ms noise attack click
//   rim   — 1.7 kHz damped sine + 2 ms noise tick
//   snare — 186 Hz tone (tau 50 ms) + one-pole high-passed noise (tau 80 ms)
//   clap  — three 4 ms noise bursts 11 ms apart + reverberant tail
//   tom   — sine sweep 220 -> 90 Hz (tau 50 ms), decay tau 160 ms
//   hat   — high-passed noise, closed tau 20 ms / open tau 110 ms
//   crash — brightly high-passed noise, decay tau 380 ms

#include "audio/drum_kit.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace addu94::audio {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Fixed-seed linear congruential generator: deterministic white noise.
class Lcg {
public:
    /// Uniform in [-1, 1).
    float next() noexcept {
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        double const u =
            static_cast<double>(state_ >> 40) / 16777216.0;  // 24 bits: 0..1
        return static_cast<float>(2.0 * u - 1.0);
    }

private:
    std::uint64_t state_{0x9E3779B97F4A7C15ull};
};

/// One-pole high-passed white noise. Smaller coefficient = brighter.
struct HpNoise {
    Lcg lcg;
    float lp{0.f};

    float operator()(float coef) noexcept {
        float const x = lcg.next();
        lp += coef * (x - lp);
        return x - lp;
    }
};

void normalize(WavData& w, float target) noexcept {
    float peak = 0.f;
    for (float v : w.samples) {
        float const a = v < 0.f ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    if (peak > 1.0e-9f) {
        float const g = target / peak;
        for (float& v : w.samples) {
            v *= g;
        }
    }
}

WavData make_base(double sr, double dur) {
    WavData w;
    w.sample_rate = static_cast<std::uint32_t>(sr);
    w.channels = 1;
    w.samples.resize(static_cast<std::size_t>(dur * sr));
    return w;
}

WavData synth_kick(double sr) {
    WavData w = make_base(sr, 0.45);
    double phase = 0.0;
    Lcg lcg;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const f = 44.0 + 110.0 * std::exp(-t / 0.06);
        phase += kTwoPi * f / sr;
        double s = std::sin(phase) * std::exp(-t / 0.14);
        if (t < 0.005) {
            s += static_cast<double>(lcg.next()) * 0.5 *
                 (1.0 - t / 0.005);
        }
        w.samples[i] = static_cast<float>(s);
    }
    normalize(w, 0.90f);
    return w;
}

WavData synth_rim(double sr) {
    WavData w = make_base(sr, 0.08);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const body =
            std::sin(kTwoPi * 1700.0 * t) * std::exp(-t / 0.008);
        double const tick = static_cast<double>(hp(0.3f)) *
                            std::exp(-t / 0.002) * 0.6;
        w.samples[i] = static_cast<float>(body + tick);
    }
    normalize(w, 0.60f);
    return w;
}

WavData synth_snare(double sr) {
    WavData w = make_base(sr, 0.35);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const tone =
            std::sin(kTwoPi * 186.0 * t) * std::exp(-t / 0.05) * 0.4;
        double const noise = static_cast<double>(hp(0.25f)) *
                             std::exp(-t / 0.08);
        w.samples[i] = static_cast<float>(tone + noise);
    }
    normalize(w, 0.80f);
    return w;
}

WavData synth_clap(double sr) {
    WavData w = make_base(sr, 0.45);
    HpNoise hp;
    constexpr double kBursts[3] = {0.0, 0.011, 0.022};
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double env = 0.0;
        for (double b : kBursts) {
            double const dt = t - b;
            if (dt >= 0.0 && dt < 0.018) {
                env += std::exp(-dt / 0.004);
            }
        }
        double const dt2 = t - 0.022;
        if (dt2 >= 0.0) {
            env += 0.7 * std::exp(-dt2 / 0.09);
        }
        w.samples[i] = static_cast<float>(static_cast<double>(hp(0.2f)) * env);
    }
    normalize(w, 0.75f);
    return w;
}

WavData synth_tom(double sr) {
    WavData w = make_base(sr, 0.5);
    double phase = 0.0;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const f = 90.0 + 130.0 * std::exp(-t / 0.05);
        phase += kTwoPi * f / sr;
        w.samples[i] =
            static_cast<float>(std::sin(phase) * std::exp(-t / 0.16));
    }
    normalize(w, 0.82f);
    return w;
}

WavData synth_closed_hat(double sr) {
    WavData w = make_base(sr, 0.1);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        w.samples[i] = static_cast<float>(static_cast<double>(hp(0.10f)) *
                                          std::exp(-t / 0.02));
    }
    normalize(w, 0.50f);
    return w;
}

WavData synth_open_hat(double sr) {
    WavData w = make_base(sr, 0.6);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        w.samples[i] = static_cast<float>(static_cast<double>(hp(0.08f)) *
                                          std::exp(-t / 0.11));
    }
    normalize(w, 0.55f);
    return w;
}

WavData synth_crash(double sr) {
    WavData w = make_base(sr, 1.4);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        w.samples[i] = static_cast<float>(static_cast<double>(hp(0.045f)) *
                                          std::exp(-t / 0.38));
    }
    normalize(w, 0.68f);
    return w;
}

}  // namespace

bool DrumKit::synthesize_fallbacks(std::string& error) {
    sampler_.set_sample_rate(sr_);
    // Bank A order (GM number of the sound in parentheses, spec §4.1):
    // 36 kick, 37 rim, 38 snare, 39 clap, 41 tom, 42 closed hat,
    // 43 open hat (the spec's GM 46 open hat, mapped onto pad 8), 49 crash.
    try {
        WavData const kits[8] = {
            synth_kick(sr_),  synth_rim(sr_),   synth_snare(sr_),
            synth_clap(sr_),  synth_tom(sr_),   synth_closed_hat(sr_),
            synth_open_hat(sr_), synth_crash(sr_),
        };
        for (int i = 0; i < 8; ++i) {
            int const note = kPadBaseNote + i;
            if (!sampler_.add_zone_data(note,
                                        kits[static_cast<std::size_t>(i)],
                                        error)) {
                return false;
            }
        }
    } catch (std::exception const& e) {
        error = std::string{"drum kit: fallback synthesis failed: "} +
                e.what();
        return false;
    }
    return true;
}

bool DrumKit::init(double sample_rate, std::string& error) {
    error.clear();
    if (!(sample_rate >= 8000.0 && sample_rate <= 192000.0)) {
        error = "drum kit: sample rate out of range 8000..192000";
        return false;
    }
    sr_ = sample_rate;
    initialized_ = synthesize_fallbacks(error);
    return initialized_;
}

bool DrumKit::load_override(int midi_note, std::string const& wav_path,
                            std::string& error) {
    return sampler_.add_zone(midi_note, wav_path, error);
}

void DrumKit::set_params(SamplerParams const& p) noexcept {
    sampler_.set_params(p);
}

void DrumKit::set_sample_rate(double sample_rate) noexcept {
    sr_ = (sample_rate >= 8000.0 && sample_rate <= 192000.0) ? sample_rate
                                                             : 48000.0;
    if (initialized_) {
        std::string err;
        initialized_ = synthesize_fallbacks(err);
    }
}

void DrumKit::note_on(int note, float velocity) noexcept {
    sampler_.note_on(9, note, velocity);  // channel 9: drum-kit mode
}

void DrumKit::render(float* left, float* right, std::uint32_t frames) noexcept {
    sampler_.render(left, right, frames);
}

Sampler& DrumKit::sampler() noexcept {
    return sampler_;
}

}  // namespace addu94::audio
