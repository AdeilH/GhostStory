// ghost_story — src/audio/drum_kit.cpp
//
// Pack synthesis implementing drum_kit.hpp. Control-thread only.
//
// Deterministic by construction: noise comes from a fixed-seed 64-bit LCG
// (never rand()), oscillators are plain phase accumulators, envelopes are
// exponentials. Two inits at the same rate produce bit-identical kits
// (tested). Each sound is normalized to its own distinct peak level so all
// eight pads stay distinguishable after the Sampler's master soft clip.
//
// Sound recipes (all mono, 0.05..1.4 s). Each recipe is parameterized by a
// small spec struct; the four built-in packs are pure parameter tables over
// the SAME DSP (user report: no pack concept existed — the kit was one
// fixed fallback). "MPC Classic" keeps the original constants so its render
// is bit-identical to the pre-pack fallback kit:
//   kick  — sine sweep f0 -> f1 Hz (sweep tau) + 5 ms noise attack click
//   rim   — damped sine + 2 ms noise tick
//   snare — tone + one-pole high-passed noise
//   clap  — three 4 ms noise bursts 11 ms apart + reverberant tail
//   tom   — sine sweep, exponential decay
//   hat   — high-passed noise, closed/open taus
//   crash — brightly high-passed noise, long decay
//
// Packs:
//   0 "MPC Classic" — original recipes (bit-identical).
//   1 "808"         — long sine sub kick, snappy rim, tight short hats,
//                     round tom.
//   2 "Lo-Fi"       — darker (lower noise brightness = larger HP coef),
//                     slower decays, lower levels.
//   3 "Techno"      — punchy short kick, bright hats, aggressive clap.
//   4 "Boom Bap"    — dusty hip-hop: warm mid kick, dry tight snare,
//                     dark dusty hats, restrained clap.
//   5 "Jungle"      — choppy breakbeat: quick subby kick, bright tight
//                     snare, razor hats, short everything.
//   6 "Trap"        — 808 sub with a touch of click, triplet-tight hats,
//                     crisp layered clap, rounded tom.
//   7 "House"       — four-on-the-floor: punchy mid kick, disco open hat,
//                     bright wide clap, classic rimshot.
//   8 "Acoustic"    — softer organic kit: low-tuned rounded kick, brushy
//                     dark snare, muted short hats, restrained crash.
//   9 "Linn 80s"    — dry 80s drum machine: clicky tight kick, snappy
//                     bright snare, ultra-tight hats, dry slap clap.
//  10 "DnB"         — drum & bass: quick subby kick, crisp snare with
//                     body, razor hats, fast crash decay.
//  11 "Dub"         — deep and slow: long dark kick, fat slow tom,
//                     splashy open hat, dark long crash, deep rim.
//  12 "Ambient"     — soft muted everything: gentle kick, whisper hats,
//                     long soft crash, lowest levels of any pack.
//  13 "Electro"     — vintage 808/909 hybrid: punchy kick with click,
//                     metallic bright hats, tight clap, hard rimshot.
//  14 "Garage"      — UK garage: mid punchy kick, sharp snappy snare,
//                     tight swung-feel hats, clicky rim.
//  15 "Breaks"      — funk breaks: mid-tuned punchy kick, fat mid snare
//                     with body, crisp hats, present clap.

#include "audio/drum_kit.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ghost_story::audio {
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

// ------------------------------------------------------------------ specs
// Defaults are the original MPC Classic constants; packs override selected
// fields with designated initializers. Every field the DSP touches is here,
// so the recipes below contain NO hidden constants.

struct KickSpec {
    double f0{154.0};        // sweep start Hz
    double f1{44.0};         // sweep end Hz
    double sweep_tau{0.06};  // pitch-sweep time constant (s)
    double amp_tau{0.14};    // amplitude decay time constant (s)
    double click{0.5};       // 5 ms noise attack click gain
    double dur{0.45};
    float level{0.90f};
};

struct RimSpec {
    double freq{1700.0};   // body sine Hz
    double body_tau{0.008};
    double tick{0.6};      // noise tick gain
    double tick_tau{0.002};
    double hp{0.3f};       // tick brightness (smaller = brighter)
    double dur{0.08};
    float level{0.60f};
};

struct SnareSpec {
    double tone{186.0};
    double tone_tau{0.05};
    double tone_gain{0.4};
    double noise_tau{0.08};
    double hp{0.25};
    double dur{0.35};
    float level{0.80f};
};

struct ClapSpec {
    double burst_gap{0.011};   // spacing of the 3 pre-tail bursts (s)
    double burst_tau{0.004};
    double burst_len{0.018};   // per-burst envelope window (s)
    double tail_gain{0.7};
    double tail_tau{0.09};
    double hp{0.2};
    double dur{0.45};
    float level{0.75f};
};

struct TomSpec {
    double f1{90.0};         // sweep end Hz
    double sweep{130.0};     // sweep extent above f1 (Hz)
    double sweep_tau{0.05};
    double amp_tau{0.16};
    double dur{0.5};
    float level{0.82f};
};

struct HatSpec {  // closed hat
    double hp{0.10};
    double tau{0.02};
    double dur{0.1};
    float level{0.50f};
};

struct OpenHatSpec {
    double hp{0.08};
    double tau{0.11};
    double dur{0.6};
    float level{0.55f};
};

struct CrashSpec {
    double hp{0.045};
    double tau{0.38};
    double dur{1.4};
    float level{0.68f};
};

struct PackSpec {
    char const* name;
    KickSpec kick;
    RimSpec rim;
    SnareSpec snare;
    ClapSpec clap;
    TomSpec tom;
    HatSpec closed_hat;
    OpenHatSpec open_hat;
    CrashSpec crash;
};

// The four built-in packs. MPC Classic = all defaults (bit-identical to the
// original hardcoded recipes); the others change only the listed fields.
// Designated initializers throughout (mixed positional/designated is an
// error, and declaration-order designators keep the tables audit-able).
PackSpec const kPackSpecs[DrumKit::pack_count()] = {
    {.name = "MPC Classic",
     .kick = {},
     .rim = {},
     .snare = {},
     .clap = {},
     .tom = {},
     .closed_hat = {},
     .open_hat = {},
     .crash = {}},
    {.name = "808",
     .kick = {.f0 = 120.0, .f1 = 40.0, .sweep_tau = 0.25, .amp_tau = 0.60,
              .click = 0.0, .dur = 1.20, .level = 0.95f},
     .rim = {.freq = 2400.0, .body_tau = 0.004, .tick = 0.8, .dur = 0.06,
             .level = 0.65f},
     .snare = {.tone = 240.0, .tone_tau = 0.03, .noise_tau = 0.05, .dur = 0.25},
     .clap = {.tail_tau = 0.12, .dur = 0.55},
     .tom = {.f1 = 70.0, .sweep = 100.0, .sweep_tau = 0.08, .amp_tau = 0.30,
             .dur = 0.80},
     .closed_hat = {.hp = 0.15, .tau = 0.012, .dur = 0.07},
     .open_hat = {.hp = 0.12, .tau = 0.09, .dur = 0.5},
     .crash = {.hp = 0.10, .tau = 0.30, .dur = 1.1}},
    {.name = "Lo-Fi",
     .kick = {.f0 = 100.0, .f1 = 38.0, .sweep_tau = 0.09, .amp_tau = 0.20,
              .click = 0.15, .dur = 0.50, .level = 0.85f},
     .rim = {.freq = 900.0, .body_tau = 0.012, .tick = 0.35, .hp = 0.10},
     .snare = {.tone = 150.0, .tone_tau = 0.07, .noise_tau = 0.14, .hp = 0.08},
     .clap = {.tail_tau = 0.14, .hp = 0.05, .dur = 0.55, .level = 0.65f},
     .tom = {.f1 = 80.0, .sweep = 100.0, .sweep_tau = 0.07, .amp_tau = 0.22,
             .dur = 0.6, .level = 0.75f},
     .closed_hat = {.hp = 0.30, .tau = 0.03, .level = 0.40f},
     .open_hat = {.hp = 0.25, .tau = 0.16, .dur = 0.7, .level = 0.45f},
     .crash = {.hp = 0.15, .tau = 0.50, .dur = 1.4, .level = 0.55f}},
    {.name = "Techno",
     .kick = {.f0 = 160.0, .f1 = 50.0, .sweep_tau = 0.035, .amp_tau = 0.09,
              .click = 0.7, .dur = 0.30, .level = 0.95f},
     .rim = {.freq = 2200.0, .tick = 0.8, .dur = 0.05},
     .snare = {.tone = 200.0, .noise_tau = 0.06, .hp = 0.35},
     .clap = {.tail_gain = 0.8, .tail_tau = 0.12, .hp = 0.10, .dur = 0.5,
              .level = 0.85f},
     .tom = {.f1 = 100.0, .sweep_tau = 0.035, .amp_tau = 0.10, .dur = 0.35},
     .closed_hat = {.hp = 0.05, .tau = 0.018, .level = 0.60f},
     .open_hat = {.hp = 0.04, .tau = 0.10, .dur = 0.55, .level = 0.65f},
     .crash = {.hp = 0.03, .tau = 0.30, .level = 0.75f}},
    {.name = "Boom Bap",
     // Dusty 90s hip-hop: the kick sits in the mid range (higher f1 than
     // MPC, moderate decay) so it thumps rather than booms; noise is dark
     // (large HP coefficients) and decays are kept dry/short.
     .kick = {.f0 = 130.0, .f1 = 48.0, .sweep_tau = 0.05, .amp_tau = 0.18,
              .click = 0.30, .dur = 0.50, .level = 0.90f},
     .rim = {.freq = 1100.0, .body_tau = 0.010, .tick = 0.40, .hp = 0.25},
     .snare = {.tone = 190.0, .tone_tau = 0.045, .tone_gain = 0.50,
               .noise_tau = 0.07, .hp = 0.20, .dur = 0.30},
     .clap = {.burst_gap = 0.010, .tail_gain = 0.5, .tail_tau = 0.07,
              .hp = 0.15, .dur = 0.40},
     .tom = {.f1 = 85.0, .sweep = 110.0, .sweep_tau = 0.05, .amp_tau = 0.18},
     .closed_hat = {.hp = 0.22, .tau = 0.025, .level = 0.45f},
     .open_hat = {.hp = 0.18, .tau = 0.13, .level = 0.50f},
     .crash = {.hp = 0.12, .tau = 0.35, .level = 0.60f}},
    {.name = "Jungle",
     // Choppy breakbeat character: everything is SHORT (the recipes are
     // cut before they can ring) so fast pad rolls read as separate hits;
     // the snare leans on bright noise, the kick is subby but quick.
     .kick = {.f0 = 150.0, .f1 = 45.0, .sweep_tau = 0.04, .amp_tau = 0.10,
              .click = 0.5, .dur = 0.30, .level = 0.90f},
     .rim = {.freq = 1900.0, .body_tau = 0.006, .tick = 0.7, .dur = 0.06},
     .snare = {.tone = 220.0, .tone_tau = 0.04, .tone_gain = 0.30,
               .noise_tau = 0.05, .hp = 0.30, .dur = 0.22},
     .clap = {.burst_gap = 0.008, .burst_len = 0.014, .tail_gain = 0.6,
              .tail_tau = 0.06, .hp = 0.25, .dur = 0.30},
     .tom = {.f1 = 110.0, .sweep = 120.0, .sweep_tau = 0.03, .amp_tau = 0.09,
             .dur = 0.25},
     .closed_hat = {.hp = 0.07, .tau = 0.015, .level = 0.55f},
     .open_hat = {.hp = 0.06, .tau = 0.08, .dur = 0.40},
     .crash = {.hp = 0.05, .tau = 0.25, .dur = 1.0, .level = 0.60f}},
    {.name = "Trap",
     // 808-style sub with just enough click to cut through small speakers,
     // triplet-ready ultra-tight hats, a crisp clap, and a round low tom.
     .kick = {.f0 = 110.0, .f1 = 38.0, .sweep_tau = 0.22, .amp_tau = 0.55,
              .click = 0.25, .dur = 1.00, .level = 0.95f},
     .rim = {.freq = 2800.0, .body_tau = 0.003, .tick = 0.9, .dur = 0.05,
             .level = 0.60f},
     .snare = {.tone = 260.0, .tone_tau = 0.025, .tone_gain = 0.30,
               .noise_tau = 0.04, .hp = 0.35, .dur = 0.18},
     .clap = {.burst_gap = 0.009, .tail_gain = 0.8, .tail_tau = 0.10,
              .hp = 0.12, .dur = 0.45, .level = 0.85f},
     .tom = {.f1 = 65.0, .sweep = 90.0, .sweep_tau = 0.06, .amp_tau = 0.25,
             .dur = 0.60},
     .closed_hat = {.hp = 0.12, .tau = 0.010, .dur = 0.06, .level = 0.55f},
     .open_hat = {.hp = 0.10, .tau = 0.07, .dur = 0.35, .level = 0.60f},
     .crash = {.hp = 0.06, .tau = 0.28, .dur = 1.0, .level = 0.70f}},
    {.name = "House",
     // Four-on-the-floor kit: a punchy mid punch kick tuned to loop at
     // 120-130 bpm, a long disco open hat, a bright wide clap, and a
     // classic 1.5 kHz rimshot on pad 2.
     .kick = {.f0 = 145.0, .f1 = 52.0, .sweep_tau = 0.045, .amp_tau = 0.13,
              .click = 0.4, .dur = 0.35, .level = 0.92f},
     .rim = {.freq = 1500.0, .body_tau = 0.007, .tick = 0.5, .dur = 0.07},
     .snare = {.tone = 210.0, .tone_tau = 0.04, .noise_tau = 0.055,
               .hp = 0.28, .dur = 0.25},
     .clap = {.burst_gap = 0.012, .tail_gain = 0.75, .tail_tau = 0.13,
              .hp = 0.08, .dur = 0.60, .level = 0.80f},
     .tom = {.f1 = 95.0, .sweep = 115.0, .sweep_tau = 0.04, .amp_tau = 0.12,
             .dur = 0.40},
     .closed_hat = {.hp = 0.06, .tau = 0.02, .level = 0.55f},
     .open_hat = {.hp = 0.05, .tau = 0.14, .dur = 0.70, .level = 0.60f},
     .crash = {.hp = 0.04, .tau = 0.32, .dur = 1.2, .level = 0.70f}},
    {.name = "Acoustic",
     // Softer organic kit: the kick is low-tuned and rounded (no click —
     // beater felt, not a transducer), the snare leans dark brushy noise,
     // hats are muted and short, and everything sits a touch quieter so
     // the kit reads "recorded in a room" rather than "programmed".
     .kick = {.f0 = 110.0, .f1 = 55.0, .sweep_tau = 0.07, .amp_tau = 0.16,
              .click = 0.0, .dur = 0.40, .level = 0.80f},
     .rim = {.freq = 1200.0, .body_tau = 0.009, .tick = 0.30, .hp = 0.15},
     .snare = {.tone = 180.0, .tone_tau = 0.06, .tone_gain = 0.30,
               .noise_tau = 0.11, .hp = 0.12, .dur = 0.32},
     .clap = {.burst_gap = 0.013, .tail_gain = 0.45, .tail_tau = 0.08,
              .hp = 0.18, .dur = 0.42, .level = 0.60f},
     .tom = {.f1 = 88.0, .sweep = 95.0, .sweep_tau = 0.06, .amp_tau = 0.20,
             .dur = 0.55},
     .closed_hat = {.hp = 0.20, .tau = 0.022, .level = 0.38f},
     .open_hat = {.hp = 0.16, .tau = 0.12, .dur = 0.55, .level = 0.42f},
     .crash = {.hp = 0.10, .tau = 0.42, .dur = 1.4, .level = 0.52f}},
    {.name = "Linn 80s",
     // Dry 80s drum-machine character: every hit is short and gated —
     // clicky tight kick, snappy bright snare, ultra-tight hats, and a
     // dry slap clap with almost no tail (the room never enters the mix).
     .kick = {.f0 = 135.0, .f1 = 55.0, .sweep_tau = 0.03, .amp_tau = 0.08,
              .click = 0.55, .dur = 0.22, .level = 0.92f},
     .rim = {.freq = 2100.0, .body_tau = 0.004, .tick = 0.85, .hp = 0.20,
             .dur = 0.05},
     .snare = {.tone = 230.0, .tone_tau = 0.03, .tone_gain = 0.35,
               .noise_tau = 0.045, .hp = 0.32, .dur = 0.18},
     .clap = {.burst_gap = 0.009, .burst_len = 0.012, .tail_gain = 0.35,
              .tail_tau = 0.045, .hp = 0.22, .dur = 0.25, .level = 0.78f},
     .tom = {.f1 = 105.0, .sweep = 120.0, .sweep_tau = 0.03, .amp_tau = 0.10,
             .dur = 0.28},
     .closed_hat = {.hp = 0.09, .tau = 0.012, .dur = 0.05, .level = 0.52f},
     .open_hat = {.hp = 0.07, .tau = 0.06, .dur = 0.30, .level = 0.55f},
     .crash = {.hp = 0.08, .tau = 0.26, .dur = 0.9, .level = 0.62f}},
    {.name = "DnB",
     // Drum & bass: the kick is subby but FAST (cut before it can drone
     // under 174 bpm rolls), the snare is crisp with a mid body, hats are
     // razor hats, and the crash decays quickly so breaks stay clean.
     .kick = {.f0 = 155.0, .f1 = 40.0, .sweep_tau = 0.032, .amp_tau = 0.11,
              .click = 0.45, .dur = 0.30, .level = 0.93f},
     .rim = {.freq = 2000.0, .body_tau = 0.005, .tick = 0.75, .hp = 0.22,
             .dur = 0.055},
     .snare = {.tone = 205.0, .tone_tau = 0.04, .tone_gain = 0.45,
               .noise_tau = 0.065, .hp = 0.30, .dur = 0.24},
     .clap = {.burst_gap = 0.008, .burst_len = 0.013, .tail_gain = 0.65,
              .tail_tau = 0.07, .hp = 0.15, .dur = 0.35, .level = 0.80f},
     .tom = {.f1 = 115.0, .sweep = 125.0, .sweep_tau = 0.025,
             .amp_tau = 0.08, .dur = 0.22},
     .closed_hat = {.hp = 0.05, .tau = 0.012, .dur = 0.055, .level = 0.58f},
     .open_hat = {.hp = 0.045, .tau = 0.065, .dur = 0.30, .level = 0.60f},
     .crash = {.hp = 0.035, .tau = 0.22, .dur = 0.85, .level = 0.62f}},
    {.name = "Dub",
     // Deep and slow: a long dark kick that blooms rather than thumps, a
     // fat slow tom, a splashy long open hat, a dark long crash, and a
     // low sub-flavored rim — the one-drop kit.
     .kick = {.f0 = 95.0, .f1 = 36.0, .sweep_tau = 0.14, .amp_tau = 0.45,
              .click = 0.10, .dur = 0.95, .level = 0.93f},
     .rim = {.freq = 700.0, .body_tau = 0.014, .tick = 0.25, .hp = 0.12,
             .dur = 0.11},
     .snare = {.tone = 165.0, .tone_tau = 0.07, .tone_gain = 0.45,
               .noise_tau = 0.10, .hp = 0.10, .dur = 0.30},
     .clap = {.burst_gap = 0.014, .tail_gain = 0.6, .tail_tau = 0.12,
              .hp = 0.07, .dur = 0.55, .level = 0.70f},
     .tom = {.f1 = 68.0, .sweep = 75.0, .sweep_tau = 0.10, .amp_tau = 0.32,
             .dur = 0.85},
     .closed_hat = {.hp = 0.24, .tau = 0.028, .level = 0.40f},
     .open_hat = {.hp = 0.14, .tau = 0.20, .dur = 0.90, .level = 0.55f},
     .crash = {.hp = 0.13, .tau = 0.55, .dur = 1.5, .level = 0.58f}},
    {.name = "Ambient",
     // Soft muted everything: gentle low-level kick, whisper hats, a long
     // soft crash, and restrained tails — a kit that sits BEHIND pads and
     // keys instead of driving the mix.
     .kick = {.f0 = 105.0, .f1 = 45.0, .sweep_tau = 0.09, .amp_tau = 0.22,
              .click = 0.05, .dur = 0.50, .level = 0.62f},
     .rim = {.freq = 950.0, .body_tau = 0.011, .tick = 0.18, .hp = 0.28,
             .level = 0.42f},
     .snare = {.tone = 160.0, .tone_tau = 0.06, .tone_gain = 0.25,
               .noise_tau = 0.09, .hp = 0.14, .dur = 0.30,
               .level = 0.60f},
     .clap = {.burst_gap = 0.012, .tail_gain = 0.4, .tail_tau = 0.10,
              .hp = 0.20, .dur = 0.45, .level = 0.50f},
     .tom = {.f1 = 82.0, .sweep = 85.0, .sweep_tau = 0.07, .amp_tau = 0.24,
             .dur = 0.60, .level = 0.60f},
     .closed_hat = {.hp = 0.26, .tau = 0.025, .level = 0.30f},
     .open_hat = {.hp = 0.20, .tau = 0.15, .dur = 0.65, .level = 0.34f},
     .crash = {.hp = 0.17, .tau = 0.60, .dur = 1.5, .level = 0.42f}},
    {.name = "Electro",
     // Vintage 808/909 hybrid: punchy kick with a hard click, metallic
     // bright hats (the highest HP coefficients of any pack), a tight
     // clap, and a hard rimshot — electro and Miami bass territory.
     .kick = {.f0 = 150.0, .f1 = 48.0, .sweep_tau = 0.04, .amp_tau = 0.12,
              .click = 0.65, .dur = 0.30, .level = 0.95f},
     .rim = {.freq = 2600.0, .body_tau = 0.003, .tick = 0.95, .hp = 0.15,
             .dur = 0.05, .level = 0.70f},
     .snare = {.tone = 240.0, .tone_tau = 0.028, .tone_gain = 0.35,
               .noise_tau = 0.05, .hp = 0.38, .dur = 0.20},
     .clap = {.burst_gap = 0.009, .burst_len = 0.013, .tail_gain = 0.6,
              .tail_tau = 0.06, .hp = 0.16, .dur = 0.30, .level = 0.82f},
     .tom = {.f1 = 100.0, .sweep = 130.0, .sweep_tau = 0.03, .amp_tau = 0.10,
             .dur = 0.30},
     .closed_hat = {.hp = 0.03, .tau = 0.014, .dur = 0.06, .level = 0.60f},
     .open_hat = {.hp = 0.025, .tau = 0.075, .dur = 0.40, .level = 0.62f},
     .crash = {.hp = 0.02, .tau = 0.28, .dur = 1.0, .level = 0.68f}},
    {.name = "Garage",
     // UK garage: a mid punchy kick that leaves room for sub bass, a
     // sharp snappy snare, tight swung-feel hats, and a clicky rim —
     // shuffle-ready and dry.
     .kick = {.f0 = 140.0, .f1 = 50.0, .sweep_tau = 0.04, .amp_tau = 0.12,
              .click = 0.45, .dur = 0.30, .level = 0.90f},
     .rim = {.freq = 1750.0, .body_tau = 0.005, .tick = 0.65, .hp = 0.24,
             .dur = 0.06},
     .snare = {.tone = 225.0, .tone_tau = 0.032, .tone_gain = 0.35,
               .noise_tau = 0.05, .hp = 0.30, .dur = 0.20},
     .clap = {.burst_gap = 0.010, .tail_gain = 0.65, .tail_tau = 0.09,
              .hp = 0.12, .dur = 0.40, .level = 0.75f},
     .tom = {.f1 = 98.0, .sweep = 110.0, .sweep_tau = 0.03, .amp_tau = 0.10,
             .dur = 0.30},
     .closed_hat = {.hp = 0.07, .tau = 0.014, .dur = 0.06, .level = 0.52f},
     .open_hat = {.hp = 0.055, .tau = 0.08, .dur = 0.35, .level = 0.55f},
     .crash = {.hp = 0.045, .tau = 0.26, .dur = 0.9, .level = 0.60f}},
    {.name = "Breaks",
     // Funk breaks: a mid-tuned punchy kick, a FAT mid snare with body
     // (the chops that carry a breakbeat), crisp hats, and a present
     // clap — sampled-funk energy.
     .kick = {.f0 = 135.0, .f1 = 52.0, .sweep_tau = 0.042, .amp_tau = 0.11,
              .click = 0.4, .dur = 0.30, .level = 0.92f},
     .rim = {.freq = 1650.0, .body_tau = 0.006, .tick = 0.6, .hp = 0.20,
             .dur = 0.065},
     .snare = {.tone = 195.0, .tone_tau = 0.055, .tone_gain = 0.55,
               .noise_tau = 0.075, .hp = 0.24, .dur = 0.28},
     .clap = {.burst_gap = 0.011, .tail_gain = 0.7, .tail_tau = 0.10,
              .hp = 0.11, .dur = 0.45, .level = 0.78f},
     .tom = {.f1 = 92.0, .sweep = 105.0, .sweep_tau = 0.04, .amp_tau = 0.13,
             .dur = 0.38},
     .closed_hat = {.hp = 0.08, .tau = 0.016, .level = 0.52f},
     .open_hat = {.hp = 0.06, .tau = 0.10, .dur = 0.45, .level = 0.56f},
     .crash = {.hp = 0.05, .tau = 0.30, .dur = 1.1, .level = 0.64f}},
};

// ---------------------------------------------------------------- recipes

WavData synth_kick(double sr, KickSpec const& s) {
    WavData w = make_base(sr, s.dur);
    double phase = 0.0;
    Lcg lcg;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const f = s.f1 + (s.f0 - s.f1) * std::exp(-t / s.sweep_tau);
        phase += kTwoPi * f / sr;
        double v = std::sin(phase) * std::exp(-t / s.amp_tau);
        if (t < 0.005) {
            v += static_cast<double>(lcg.next()) * s.click *
                 (1.0 - t / 0.005);
        }
        w.samples[i] = static_cast<float>(v);
    }
    normalize(w, s.level);
    return w;
}

WavData synth_rim(double sr, RimSpec const& s) {
    WavData w = make_base(sr, s.dur);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const body =
            std::sin(kTwoPi * s.freq * t) * std::exp(-t / s.body_tau);
        double const tick = static_cast<double>(hp(static_cast<float>(s.hp))) *
                            std::exp(-t / s.tick_tau) * s.tick;
        w.samples[i] = static_cast<float>(body + tick);
    }
    normalize(w, s.level);
    return w;
}

WavData synth_snare(double sr, SnareSpec const& s) {
    WavData w = make_base(sr, s.dur);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const tone =
            std::sin(kTwoPi * s.tone * t) * std::exp(-t / s.tone_tau) *
            s.tone_gain;
        double const noise = static_cast<double>(hp(static_cast<float>(s.hp))) *
                             std::exp(-t / s.noise_tau);
        w.samples[i] = static_cast<float>(tone + noise);
    }
    normalize(w, s.level);
    return w;
}

WavData synth_clap(double sr, ClapSpec const& s) {
    WavData w = make_base(sr, s.dur);
    HpNoise hp;
    constexpr int kBurstCount = 3;
    double const bursts[kBurstCount] = {0.0, s.burst_gap, 2.0 * s.burst_gap};
    double const tail_start = 2.0 * s.burst_gap;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double env = 0.0;
        for (double b : bursts) {
            double const dt = t - b;
            if (dt >= 0.0 && dt < s.burst_len) {
                env += std::exp(-dt / s.burst_tau);
            }
        }
        double const dt2 = t - tail_start;
        if (dt2 >= 0.0) {
            env += s.tail_gain * std::exp(-dt2 / s.tail_tau);
        }
        w.samples[i] =
            static_cast<float>(static_cast<double>(hp(static_cast<float>(s.hp))) *
                               env);
    }
    normalize(w, s.level);
    return w;
}

WavData synth_tom(double sr, TomSpec const& s) {
    WavData w = make_base(sr, s.dur);
    double phase = 0.0;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        double const f = s.f1 + s.sweep * std::exp(-t / s.sweep_tau);
        phase += kTwoPi * f / sr;
        w.samples[i] =
            static_cast<float>(std::sin(phase) * std::exp(-t / s.amp_tau));
    }
    normalize(w, s.level);
    return w;
}

WavData synth_closed_hat(double sr, HatSpec const& s) {
    WavData w = make_base(sr, s.dur);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        w.samples[i] =
            static_cast<float>(static_cast<double>(hp(static_cast<float>(s.hp))) *
                               std::exp(-t / s.tau));
    }
    normalize(w, s.level);
    return w;
}

WavData synth_open_hat(double sr, OpenHatSpec const& s) {
    WavData w = make_base(sr, s.dur);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        w.samples[i] =
            static_cast<float>(static_cast<double>(hp(static_cast<float>(s.hp))) *
                               std::exp(-t / s.tau));
    }
    normalize(w, s.level);
    return w;
}

WavData synth_crash(double sr, CrashSpec const& s) {
    WavData w = make_base(sr, s.dur);
    HpNoise hp;
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        double const t = static_cast<double>(i) / sr;
        w.samples[i] =
            static_cast<float>(static_cast<double>(hp(static_cast<float>(s.hp))) *
                               std::exp(-t / s.tau));
    }
    normalize(w, s.level);
    return w;
}

}  // namespace

char const* DrumKit::pack_name(int idx) noexcept {
    if (idx < 0 || idx >= pack_count()) {
        return nullptr;
    }
    return kPackSpecs[static_cast<std::size_t>(idx)].name;
}

bool DrumKit::synthesize_pack(int idx, std::string& error) {
    Sampler& s = samplers_[static_cast<std::size_t>(idx)];
    s.set_sample_rate(sr_);
    PackSpec const& spec = kPackSpecs[static_cast<std::size_t>(idx)];
    // Bank A order (GM number of the sound in parentheses, spec §4.1):
    // 36 kick, 37 rim, 38 snare, 39 clap, 41 tom, 42 closed hat,
    // 43 open hat (the spec's GM 46 open hat, mapped onto pad 8), 49 crash.
    try {
        WavData const kits[8] = {
            synth_kick(sr_, spec.kick),       synth_rim(sr_, spec.rim),
            synth_snare(sr_, spec.snare),     synth_clap(sr_, spec.clap),
            synth_tom(sr_, spec.tom),         synth_closed_hat(sr_, spec.closed_hat),
            synth_open_hat(sr_, spec.open_hat), synth_crash(sr_, spec.crash),
        };
        for (int i = 0; i < 8; ++i) {
            int const note = kPadBaseNote + i;
            if (!s.add_zone_data(note, kits[static_cast<std::size_t>(i)],
                                 error)) {
                return false;
            }
        }
    } catch (std::exception const& e) {
        error = std::string{"drum kit: pack synthesis failed: "} + e.what();
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
    initialized_ = true;
    for (int p = 0; p < pack_count(); ++p) {
        if (!synthesize_pack(p, error)) {
            initialized_ = false;
            return false;
        }
    }
    return true;
}

bool DrumKit::load_pack(int idx, std::string& error) {
    error.clear();
    if (!initialized_) {
        error = "drum kit: not initialized";
        return false;
    }
    if (idx < 0 || idx >= pack_count()) {
        error = "drum kit: pack index out of range 0.." +
                std::to_string(pack_count() - 1);
        return false;
    }
    // The RT path only ever reads the index (int-sized atomic store) — no
    // zone-table mutation, no shared_ptr writes, no allocation here.
    active_pack_.store(idx, std::memory_order_relaxed);
    return true;
}

bool DrumKit::load_override(int midi_note, std::string const& wav_path,
                            std::string& error) {
    return samplers_[static_cast<std::size_t>(
                        active_pack_.load(std::memory_order_relaxed))]
        .add_zone(midi_note, wav_path, error);
}

bool DrumKit::load_override_data(int midi_note, WavData const& data,
                                 std::string& error) {
    return samplers_[static_cast<std::size_t>(
                        active_pack_.load(std::memory_order_relaxed))]
        .add_zone_data(midi_note, data, error);
}

void DrumKit::set_params(SamplerParams const& p) noexcept {
    for (auto& s : samplers_) {
        s.set_params(p);
    }
}

void DrumKit::set_sample_rate(double sample_rate) noexcept {
    sr_ = (sample_rate >= 8000.0 && sample_rate <= 192000.0) ? sample_rate
                                                             : 48000.0;
    if (initialized_) {
        std::string err;
        for (int p = 0; p < pack_count(); ++p) {
            if (!synthesize_pack(p, err)) {
                initialized_ = false;  // OOM downgrade (see header)
                return;
            }
        }
    }
}

void DrumKit::note_on(int note, float velocity) noexcept {
    // Single relaxed atomic load: the pack choice is latched per hit. A pack
    // switch between this and render() means the hit lives in the previous
    // pack's sampler and is silent for one buffer — a one-hit glitch on a
    // rare user action, accepted and documented (see header).
    samplers_[static_cast<std::size_t>(
                  active_pack_.load(std::memory_order_relaxed))]
        .note_on(9, note, velocity);  // channel 9: drum-kit mode
}

void DrumKit::render(float* left, float* right, std::uint32_t frames) noexcept {
    samplers_[static_cast<std::size_t>(
                  active_pack_.load(std::memory_order_relaxed))]
        .render(left, right, frames);
}

Sampler& DrumKit::sampler() noexcept {
    return samplers_[static_cast<std::size_t>(
        active_pack_.load(std::memory_order_relaxed))];
}

}  // namespace ghost_story::audio
