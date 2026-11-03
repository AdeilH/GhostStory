// addu94 — src/audio/synth.cpp
//
// Subtractive synth DSP implementing the contract in synth.hpp.
//
// RT contract: no allocation, no locks, no logging, no exceptions on any
// public entry point. All state lives in the pre-allocated voice pool plus
// the file-local `g_ext` side table below.
//
// Why a side table? `Voice` (fixed by the header) carries two SVF state
// floats and one phase pair. Three things need more room:
//   1. unison copies 1..2 of both oscillators (2 extra phase pairs),
//   2. true LP24 = two cascaded SVF sections (2 extra state floats),
//   3. stereo: the unison copies are panned BEFORE the filter, so the left
//      and right signals are different linear combinations and must be
//      filtered with independent state (2 extra state floats).
// `g_ext` is a fixed-size, zero-initialized array indexed by voice index —
// it is never resized, so it is RT-safe. Limitation: it is process-global,
// so running TWO Synth instances concurrently on the same voice indices
// would clobber each other's unison/filter state. The engine owns exactly
// one Synth; documented as an accepted deviation.
//
// SVF formulation (Chamberlin state-variable, per sample, per channel):
//     hp = x - lp1 - d*bp1
//     bp1 += f*hp
//     lp1 += f*bp1
// with f = 2*sin(pi*fc/sr) clamped to [0.01, 0.93] and d = 1/Q (damping,
// Q = resonance clamped to [0.5, 10]). Outputs: LP = lp1, HP = hp,
// BP = bp1. LP24 feeds lp1 into a second identical section (states lp2,
// bp2) and takes its lp output. Internal states are hard-clamped to
// [-8, 8] so extreme f/d combinations cannot blow up to inf/NaN (mild
// distortion instead of a destroyed mix — accepted trade-off).

#include "audio/synth.hpp"

#include <cmath>

namespace addu94::audio {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = 3.141592653589793238462643383279;
constexpr float kEnvSilence = 1.0e-4f;  // end-of-release threshold
constexpr float kStateClamp = 8.0f;     // SVF state hard clamp
constexpr float kMasterGain = 0.5f;     // headroom for polyphony

// Internal env_stage extension beyond the header's 0..3:
//   4 = fade tail. The transition into it (from release) renders env*0.5;
//       this stage renders 0 and deactivates. Together: the 2-sample linear
//       fade (full level -> half -> zero) that kills the click when a fast
//       or zero release drops the envelope below the silence threshold.
constexpr int kStageFade = 4;

// Per-voice state that does not fit into the fixed Voice struct.
struct VoiceExt {
    double phase1b[2]{0.0, 0.0};  // unison copies 1,2 — osc1
    double phase2b[2]{0.0, 0.0};  // unison copies 1,2 — osc2
    float lp1r{0.f};              // right-channel SVF section 1, low state
    float bp1r{0.f};              // right-channel SVF section 1, band state
    float lp2[2]{0.f, 0.f};       // LP24 section 2, low state  [L, R]
    float bp2[2]{0.f, 0.f};       // LP24 section 2, band state  [L, R]
};

VoiceExt g_ext[kMaxVoices];  // indexed by voice index, see note above

inline float clampf(float x, float lo, float hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline void clamp_state(float& s) noexcept {
    if (s > kStateClamp) {
        s = kStateClamp;
    } else if (s < -kStateClamp) {
        s = -kStateClamp;
    }
}

inline void advance_phase(double& phase, double inc) noexcept {
    phase += inc;
    // inc < 0.5 by construction (cutoff of audible range), one wrap is enough.
    if (phase >= 1.0) {
        phase -= 1.0;
    } else if (phase < 0.0) {
        phase += 1.0;
    }
}

inline float wave_value(Waveform w, double phase) noexcept {
    switch (w) {
    case Waveform::Saw:
        return static_cast<float>(2.0 * phase - 1.0);
    case Waveform::Square:
        return phase < 0.5 ? 1.0f : -1.0f;
    case Waveform::Triangle:
        return static_cast<float>(1.0 - 4.0 * std::fabs(phase - 0.5));
    case Waveform::Sine:
        return static_cast<float>(std::sin(kTwoPi * phase));
    case Waveform::Pulse25:
        return phase < 0.25 ? 1.0f : -1.0f;
    }
    return 0.0f;
}

/// One Chamberlin SVF pass for one channel. `lp`/`bp` are section-1 states,
/// `lp24`/`bp24` the (unused for 12 dB modes) section-2 states.
inline float svf_run(float x, float& lp, float& bp, float& lp24, float& bp24,
                     FilterMode mode, float f, float d) noexcept {
    float const hp = x - lp - d * bp;
    bp += f * hp;
    lp += f * bp;
    clamp_state(bp);
    clamp_state(lp);
    float y = 0.f;
    switch (mode) {
    case FilterMode::LP12:
        y = lp;
        break;
    case FilterMode::HP12:
        y = hp;
        break;
    case FilterMode::BP12:
        y = bp;
        break;
    case FilterMode::LP24: {
        float const hp2 = lp - lp24 - d * bp24;
        bp24 += f * hp2;
        lp24 += f * bp24;
        clamp_state(bp24);
        clamp_state(lp24);
        y = lp24;
        break;
    }
    }
    return y;
}

}  // namespace

Synth::Synth() noexcept {
    voices_ = {};
    for (auto& e : g_ext) {
        e = VoiceExt{};
    }
}

void Synth::set_sample_rate(double sr) noexcept {
    sr_ = (sr >= 8000.0 && sr <= 192000.0) ? sr : 48000.0;
}

void Synth::set_params(SynthParams const& p) noexcept {
    p_ = p;
    // Defensive clamps so a bad UI value can never destabilize the DSP.
    p_.osc_mix = clampf(p_.osc_mix, 0.f, 1.f);
    p_.osc2_semitones = p_.osc2_semitones < -24   ? -24
                        : p_.osc2_semitones > 24  ? 24
                                                  : p_.osc2_semitones;
    p_.osc2_detune_cents = clampf(p_.osc2_detune_cents, -100.f, 100.f);
    p_.unison_voices = p_.unison_voices < 1 ? 1
                        : p_.unison_voices > 3 ? 3
                                               : p_.unison_voices;
    p_.unison_spread_cents = clampf(p_.unison_spread_cents, 0.f, 100.f);
    p_.attack = clampf(p_.attack, 0.f, 60.f);
    p_.decay = clampf(p_.decay, 0.f, 60.f);
    p_.sustain = clampf(p_.sustain, 0.f, 1.f);
    p_.release = clampf(p_.release, 0.f, 60.f);
    p_.cutoff_base_hz = clampf(p_.cutoff_base_hz, 20.f, 18000.f);
    p_.cutoff_keytrack = clampf(p_.cutoff_keytrack, 0.f, 1.f);
    p_.resonance = clampf(p_.resonance, 0.5f, 10.f);
    p_.lfo_rate_hz = clampf(p_.lfo_rate_hz, 0.05f, 20.f);
    p_.lfo_depth = clampf(p_.lfo_depth, 0.f, 1.f);
}

void Synth::note_on(int channel, int note, float velocity) noexcept {
    if (note < 0 || note > 127 || channel < 0 || channel > 15) {
        return;
    }
    // Prefer a free voice, scanning round-robin from oldest_hint_ so voices
    // age evenly; otherwise steal the quietest (smallest env) voice, again
    // in round-robin order so ties go to the oldest.
    int idx = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        int const j = (oldest_hint_ + i) % kMaxVoices;
        if (!voices_[static_cast<std::size_t>(j)].active) {
            idx = j;
            break;
        }
    }
    if (idx < 0) {
        float best = 1e30f;
        for (int i = 0; i < kMaxVoices; ++i) {
            int const j = (oldest_hint_ + i) % kMaxVoices;
            if (voices_[static_cast<std::size_t>(j)].env < best) {
                best = voices_[static_cast<std::size_t>(j)].env;
                idx = j;
            }
        }
    }
    if (idx < 0) {
        return;  // unreachable with kMaxVoices > 0
    }

    Voice& v = voices_[static_cast<std::size_t>(idx)];
    VoiceExt& e = g_ext[static_cast<std::size_t>(idx)];

    // Retrigger a sounding (e.g. still-releasing) voice from its current
    // envelope level instead of 0 — avoids a click on steal.
    float const start_env = v.active ? v.env : 0.f;

    v.active = true;
    v.note = static_cast<std::uint8_t>(note);
    v.channel = static_cast<std::uint8_t>(channel);
    v.velocity = clampf(velocity, 0.f, 1.f);
    v.phase1 = 0.0;
    v.phase2 = 0.0;
    e.phase1b[0] = e.phase1b[1] = 0.0;
    e.phase2b[0] = e.phase2b[1] = 0.0;
    v.env = start_env;
    v.env_stage = 0;  // attack
    v.filter_state1 = 0.f;
    v.filter_state2 = 0.f;
    e.lp1r = e.bp1r = 0.f;
    e.lp2[0] = e.lp2[1] = 0.f;
    e.bp2[0] = e.bp2[1] = 0.f;
    v.lfo_phase = 0.f;
    v.last_amp = 0.f;

    oldest_hint_ = (idx + 1) % kMaxVoices;
}

void Synth::note_off(int channel, int note) noexcept {
    if (channel < 0 || channel > 15) {
        return;
    }
    for (auto& v : voices_) {
        if (v.active && v.channel == channel && v.note == note &&
            v.env_stage < 3) {
            v.env_stage = 3;  // release (stages 4/5 are already fading)
        }
    }
}

void Synth::all_sound_off() noexcept {
    // Instant deactivate: every voice goes silent at the next render with NO
    // fade, so a click is possible by design. This is the panic / track-mute
    // path, where immediate silence beats click-free ramping.
    for (auto& v : voices_) {
        v = Voice{};
    }
    for (auto& e : g_ext) {
        e = VoiceExt{};
    }
    oldest_hint_ = 0;
}

void Synth::render(float* left, float* right, std::uint32_t frames) noexcept {
    if (left == nullptr || right == nullptr) {
        return;
    }
    // Overwrite semantics: the engine hands us fresh (zeroed) buffers; we
    // zero them again here so the guarantee "no active voices => exact
    // zeros" holds regardless of caller buffer state.
    for (std::uint32_t i = 0; i < frames; ++i) {
        left[i] = 0.f;
        right[i] = 0.f;
    }
    if (frames == 0) {
        return;
    }

    double const dt = 1.0 / sr_;

    // Per-block envelope factors (RT-safe: a handful of exp() calls per
    // render, never per sample).
    //   attack: linear ramp, 1/(attack*sr) per sample.
    //   decay : env = sustain + (env - sustain)*k, k reaches 0.1% of the
    //           initial gap after `decay` seconds  => k = 0.001^(dt/decay).
    //   release: env reaches 0.0001 after `release` seconds
    //           => k = 0.0001^(dt/release).
    float const attack_rate = p_.attack > 1.0e-6f
                                  ? static_cast<float>(1.0 / (static_cast<double>(p_.attack) * sr_))
                                  : 1.0e9f;
    float const decay_factor =
        p_.decay > 1.0e-6f
            ? static_cast<float>(std::exp(-6.907755279 * dt / static_cast<double>(p_.decay)))
            : 0.f;
    float const release_factor =
        p_.release > 1.0e-6f
            ? static_cast<float>(std::exp(-9.210340372 * dt / static_cast<double>(p_.release)))
            : 0.f;
    float const sustain = p_.sustain;

    // LFO increment (sine phase in [0,1), cheap per-sample advance).
    float const lfo_inc = static_cast<float>(static_cast<double>(p_.lfo_rate_hz) / sr_);

    // Unison layout: up to 3 detuned copies spread symmetrically in cents,
    // panned at constant power across the stereo field.
    int const n = p_.unison_voices;
    float const s = p_.unison_spread_cents;
    double const sd = static_cast<double>(s);
    double const cents[3] = {
        n == 2 ? -0.5 * sd : (n == 3 ? -sd : 0.0),
        n == 3 ? 0.0 : (n == 2 ? 0.5 * sd : 0.0),
        n == 3 ? sd : 0.0,
    };
    float const inv_n = 1.0f / static_cast<float>(n);
    float pan_l[3] = {0.70710678f, 0.70710678f, 0.70710678f};
    float pan_r[3] = {0.70710678f, 0.70710678f, 0.70710678f};
    for (int c = 0; c < n; ++c) {
        double const pos = n == 1 ? 0.0 : (2.0 * c / (n - 1) - 1.0);  // -1..1
        double const ang = (pos + 1.0) * (kPi / 4.0);  // constant-power pan
        pan_l[c] = static_cast<float>(std::cos(ang));
        pan_r[c] = static_cast<float>(std::sin(ang));
    }

    float const mix = p_.osc_mix;
    float const mix1 = 1.0f - mix;
    FilterMode const mode = p_.filter_mode;
    LfoTarget const lfo_target = p_.lfo_target;
    float const lfo_depth = p_.lfo_depth;
    float const d = 1.0f / p_.resonance;  // SVF damping
    double const semis2 = static_cast<double>(p_.osc2_semitones) * 100.0 +
                          static_cast<double>(p_.osc2_detune_cents);

    for (int vi = 0; vi < kMaxVoices; ++vi) {
        Voice& v = voices_[static_cast<std::size_t>(vi)];
        if (!v.active) {
            continue;
        }
        VoiceExt& e = g_ext[vi];

        double const base_hz =
            440.0 * std::pow(2.0, (static_cast<double>(v.note) - 69.0) / 12.0);

        // Keytracked cutoff: fc = clamp(base * 2^(keytrack*(note-60)/12)).
        double const kt = static_cast<double>(p_.cutoff_keytrack) *
                          (static_cast<double>(v.note) - 60.0) / 12.0;
        float const cutoff = clampf(
            static_cast<float>(static_cast<double>(p_.cutoff_base_hz) *
                               std::pow(2.0, kt)),
            20.f, 18000.f);
        float const f_base = clampf(
            static_cast<float>(2.0 * std::sin(kPi * static_cast<double>(cutoff) / sr_)), 0.01f,
            0.93f);

        // Per-copy phase increments (copy 0 lives in the Voice struct).
        double inc1[3] = {0.0, 0.0, 0.0};
        double inc2[3] = {0.0, 0.0, 0.0};
        for (int c = 0; c < n; ++c) {
            inc1[c] = base_hz * std::pow(2.0, cents[c] / 1200.0) / sr_;
            inc2[c] = base_hz *
                      std::pow(2.0, (semis2 + cents[c]) / 1200.0) / sr_;
        }

        for (std::uint32_t i = 0; i < frames; ++i) {
            // --- LFO (per-voice sine phase, cheap per-sample advance) ---
            float const lfo = static_cast<float>(
                std::sin(kTwoPi * static_cast<double>(v.lfo_phase)));
            v.lfo_phase += lfo_inc;
            if (v.lfo_phase >= 1.f) {
                v.lfo_phase -= 1.f;
            }
            float pm = 1.0f;        // pitch modulation factor on increment
            float f = f_base;       // SVF coefficient (modulated for Filter)
            float amp_mod = 1.0f;   // gain scale (modulated for Amp)
            switch (lfo_target) {
            case LfoTarget::Filter: {
                float const fc = clampf(
                    cutoff * static_cast<float>(std::pow(
                                 2.0, static_cast<double>(lfo) *
                                          static_cast<double>(lfo_depth))),
                    20.f, 18000.f);
                f = clampf(
                    static_cast<float>(2.0 * std::sin(kPi * static_cast<double>(fc) / sr_)), 0.01f,
                    0.93f);
                break;
            }
            case LfoTarget::Pitch:
                // Vibrato: depth interpreted as semitones peak deviation.
                pm = static_cast<float>(std::pow(
                    2.0, static_cast<double>(lfo) *
                             static_cast<double>(lfo_depth) / 12.0));
                break;
            case LfoTarget::Amp:
                amp_mod = clampf(1.0f + lfo * lfo_depth, 0.f, 2.f);
                break;
            case LfoTarget::Off:
                break;
            }

            // --- Oscillators: unison copies panned before the mix, so the
            // filter input differs per channel and is filtered separately.
            double o1l = 0.0, o1r = 0.0, o2l = 0.0, o2r = 0.0;
            for (int c = 0; c < n; ++c) {
                double& ph1 = c == 0 ? v.phase1 : e.phase1b[c - 1];
                advance_phase(ph1, static_cast<double>(inc1[c]) * static_cast<double>(pm));
                float const w1 = wave_value(p_.osc1, ph1);
                double& ph2 = c == 0 ? v.phase2 : e.phase2b[c - 1];
                advance_phase(ph2, static_cast<double>(inc2[c]) * static_cast<double>(pm));
                float const w2 = wave_value(p_.osc2, ph2);
                double const pl = static_cast<double>(pan_l[c]);
                double const pr = static_cast<double>(pan_r[c]);
                double const wd1 = static_cast<double>(w1);
                double const wd2 = static_cast<double>(w2);
                o1l += pl * wd1;
                o1r += pr * wd1;
                o2l += pl * wd2;
                o2r += pr * wd2;
            }
            double const mix1d = static_cast<double>(mix1);
            double const mixd = static_cast<double>(mix);
            double const inv_nd = static_cast<double>(inv_n);
            float const in_l = static_cast<float>(
                (o1l * mix1d + o2l * mixd) * inv_nd);  // osc_mix crossfade
            float const in_r = static_cast<float>(
                (o1r * mix1d + o2r * mixd) * inv_nd);

            // --- Filter (left uses the Voice header states, right the ext
            // states; LP24 cascades a second section per channel). ---
            float const fl = svf_run(in_l, v.filter_state1, v.filter_state2,
                                     e.lp2[0], e.bp2[0], mode, f, d);
            float const fr = svf_run(in_r, e.lp1r, e.bp1r, e.lp2[1], e.bp2[1],
                                     mode, f, d);

            // --- Amp envelope state machine ---
            float gain = 0.f;
            switch (v.env_stage) {
            case 0:  // attack: linear to 1.0
                v.env += attack_rate;
                if (v.env >= 1.f) {
                    v.env = 1.f;
                    v.env_stage = sustain >= 0.999f ? 2 : 1;
                }
                gain = v.env;
                break;
            case 1: {  // decay: exponential towards sustain
                v.env = sustain + (v.env - sustain) * decay_factor;
                if (v.env - sustain < 1.0e-3f || v.env < sustain) {
                    v.env = sustain;
                    v.env_stage = 2;
                }
                gain = v.env;
                break;
            }
            case 2:  // sustain hold
                gain = v.env;
                break;
            case 3: {  // release: exponential from current level
                float const next = v.env * release_factor;
                if (next < kEnvSilence) {
                    v.env_stage = kStageFade;  // 2-sample fade tail
                    gain = v.env * 0.5f;
                } else {
                    v.env = next;
                    gain = v.env;
                }
                break;
            }
            default:  // kStageFade: final zero sample, then deactivate
                gain = 0.f;
                v.active = false;
                v.last_amp = 0.f;
                break;
            }
            if (!v.active) {
                continue;  // voice finished mid-block; nothing more to add
            }

            float const amp =
                gain * v.velocity * amp_mod * kMasterGain;
            left[i] += fl * amp;
            right[i] += fr * amp;
            v.last_amp = amp;
        }
    }
}

int Synth::active_voices() const noexcept {
    int count = 0;
    for (auto const& v : voices_) {
        if (v.active) {
            ++count;
        }
    }
    return count;
}

}  // namespace addu94::audio
