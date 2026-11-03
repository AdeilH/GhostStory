// addu94 — src/audio/sampler.cpp
//
// Sampler DSP implementing the contract in sampler.hpp.
//
// RT notes:
//  - note_on: fixed-voice scan, shared_ptr refcount bump, no allocation.
//  - render: per-voice linear interpolation + exponential decay envelope +
//    linear choke fade, then one tanh() soft clip per output frame.
//  - The decay envelope is env *= exp(-dt/tau) with tau = (sample duration
//    at the current playback rate) * pad_decay_scale — scale 1 means the
//    envelope reaches e^-1 (~37%) exactly at the natural end of the sample.
//    A 64-frame source-side ramp to zero at the very end of the sample
//    removes the truncation click of one-shot playback.
//  - Choke: linear fade from the voice's current level to 0 over 2 ms
//    (fade_step = fade / (0.002 * sr) at trigger time; an already-fading
//    voice keeps the faster of the two steps).

#include "audio/sampler.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace addu94::audio {
namespace {

constexpr float kEnvSilence = 1.0e-4f;   // deanimate voice below this
constexpr std::uint32_t kEndRampFrames = 64;  // source frames of end fade
constexpr int kChokeFadeMsNum = 2;       // 2 ms choke fade

inline float clampf(float x, float lo, float hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline double clampd(double x, double lo, double hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

}  // namespace

Sampler::Sampler() noexcept = default;

bool Sampler::add_zone(int midi_note, std::string const& wav_path,
                       std::string& error) {
    std::optional<WavData> wav = wav_load(wav_path, error);
    if (!wav.has_value()) {
        return false;
    }
    return add_zone_data(midi_note, *wav, error);
}

bool Sampler::add_zone_data(int midi_note, WavData const& data,
                            std::string& error) {
    if (midi_note < 0 || midi_note > 127) {
        error = "sampler: midi note out of range 0..127";
        return false;
    }
    if (data.channels != 1 && data.channels != 2) {
        error = "sampler: zone channels must be mono or stereo";
        return false;
    }
    if (data.sample_rate < 1u || data.sample_rate > 192000u) {
        error = "sampler: zone sample rate out of range";
        return false;
    }
    if (data.samples.empty() || data.samples.size() % data.channels != 0) {
        error = "sampler: zone samples empty or not frame-aligned";
        return false;
    }
    try {
        auto sd = std::make_shared<SampleData>();
        sd->sample_rate = data.sample_rate;
        sd->channels = data.channels;
        sd->frames = static_cast<std::uint32_t>(
            data.samples.size() / static_cast<std::size_t>(data.channels));
        sd->samples = data.samples;
        std::size_t const slot = static_cast<std::size_t>(midi_note);
        if (zones_[slot] != nullptr) {
            retired_.push_back(std::move(zones_[slot]));  // never freed on RT
        }
        zones_[slot] = std::move(sd);
    } catch (std::exception const& e) {
        error = std::string{"sampler: zone allocation failed: "} + e.what();
        return false;
    }
    return true;
}

void Sampler::set_params(SamplerParams const& p) noexcept {
    p_ = p;
    p_.gain = clampf(p_.gain, 0.f, 4.f);
    for (int i = 0; i < 8; ++i) {
        std::size_t const pi = static_cast<std::size_t>(i);
        int& t = p_.pad_tuning_semitones[pi];
        t = t < -24 ? -24 : (t > 24 ? 24 : t);
        // 0 / unset stays 0, which note_on interprets as "natural".
        float& d = p_.pad_decay_scale[pi];
        d = d <= 0.f ? 0.f : clampf(d, 0.1f, 4.f);
        int& g = p_.pad_choke_group[pi];
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
    }
}

void Sampler::set_sample_rate(double sr) noexcept {
    sr_ = (sr >= 8000.0 && sr <= 192000.0) ? sr : 48000.0;
}

void Sampler::note_on(int channel, int note, float velocity) noexcept {
    if (note < 0 || note > 127 || channel < 0 || channel > 15) {
        return;
    }

    // Pad mapping: 36..43 -> pads 0..7; 44..51 -> the same pad zones
    // (stored at 36..43) played with an extra +12 semitone shift.
    int pad = -1;
    int tune = 0;
    int zone_note = note;
    if (note >= kPadBaseNote && note < kPadBaseNote + kPadCount) {
        pad = note - kPadBaseNote;
    } else if (note >= kPadBaseNote + kPadCount &&
               note < kPadBaseNote + 2 * kPadCount) {
        pad = note - kPadBaseNote - kPadCount;
        tune = 12;
        zone_note = kPadBaseNote + pad;
    }

    std::shared_ptr<SampleData const> const& zone =
        zones_[static_cast<std::size_t>(zone_note)];
    if (zone == nullptr) {
        return;
    }
    SampleData const& s = *zone;

    float decay_scale = 1.f;
    int choke = 0;
    if (pad >= 0) {
        std::size_t const pi = static_cast<std::size_t>(pad);
        tune += p_.pad_tuning_semitones[pi];
        float const ds = p_.pad_decay_scale[pi];
        decay_scale = ds <= 0.f ? 1.f : ds;
        choke = p_.pad_choke_group[pi];
    }

    // Choke: fade same-group voices out over 2 ms (linear from current level).
    if (choke > 0) {
        float const fs = static_cast<float>(
            1.0 / ((static_cast<double>(kChokeFadeMsNum) / 1000.0) * sr_));
        for (auto& v : voices_) {
            if (v.active && v.choke_group == static_cast<std::uint8_t>(choke)) {
                float const step = v.fade * fs;
                if (step > v.fade_step) {
                    v.fade_step = step;
                }
            }
        }
    }

    // Prefer a free voice (round-robin from oldest), else steal the
    // quietest (env * fade) in round-robin order.
    int idx = -1;
    for (int i = 0; i < kSamplerMaxVoices; ++i) {
        int const j = (oldest_hint_ + i) % kSamplerMaxVoices;
        if (!voices_[static_cast<std::size_t>(j)].active) {
            idx = j;
            break;
        }
    }
    if (idx < 0) {
        float best = 1e30f;
        for (int i = 0; i < kSamplerMaxVoices; ++i) {
            int const j = (oldest_hint_ + i) % kSamplerMaxVoices;
            Voice const& v = voices_[static_cast<std::size_t>(j)];
            float const q = v.env * v.fade;
            if (q < best) {
                best = q;
                idx = j;
            }
        }
    }
    if (idx < 0) {
        return;  // unreachable with kSamplerMaxVoices > 0
    }

    Voice& v = voices_[static_cast<std::size_t>(idx)];

    float const vel = clampf(velocity, 0.f, 1.f);
    double const pitch =
        std::pow(2.0, static_cast<double>(tune) / 12.0);
    double const rate =
        static_cast<double>(s.sample_rate) / sr_ * pitch;
    float const rate_f = static_cast<float>(clampd(rate, 1.0 / 64.0, 64.0));

    // Decay envelope: tau (seconds) = heard duration * decay_scale, so the
    // envelope reaches e^-1 at the natural end when scale == 1.
    double const natural =
        static_cast<double>(s.frames) / static_cast<double>(s.sample_rate) /
        static_cast<double>(rate_f);
    double const tau = natural * static_cast<double>(decay_scale);
    float const decay_factor =
        tau > 1.0e-6
            ? static_cast<float>(std::exp(-1.0 / (tau * sr_)))
            : 0.f;

    v.active = true;
    v.note = static_cast<std::uint8_t>(note);
    v.channel = static_cast<std::uint8_t>(channel);
    v.choke_group = static_cast<std::uint8_t>(choke);
    v.gain = std::pow(vel, 1.5f) * p_.gain;
    v.rate = rate_f;
    v.decay_factor = decay_factor;
    v.env = 1.f;
    v.fade = 1.f;
    v.fade_step = 0.f;
    v.pos = 0.0;
    v.sample = zone;  // shared_ptr copy: atomic refcount bump, no lock

    oldest_hint_ = (idx + 1) % kSamplerMaxVoices;
}

void Sampler::note_off(int, int) noexcept {
    // One-shot semantics: samples play to their natural end regardless of
    // note_off. Choke groups act on note_on (see note_on).
}

void Sampler::all_sound_off() noexcept {
    // Voice assignment releases only the voice's reference; the zone table
    // (or retirement list) still owns the sample memory, so nothing is
    // deallocated here on the RT thread.
    for (auto& v : voices_) {
        v = Voice{};
    }
    oldest_hint_ = 0;
}

void Sampler::render(float* left, float* right, std::uint32_t frames) noexcept {
    if (left == nullptr || right == nullptr) {
        return;
    }
    // Overwrite semantics (same guarantee as Synth): exact zeros when no
    // voice is active, regardless of caller buffer state.
    for (std::uint32_t i = 0; i < frames; ++i) {
        left[i] = 0.f;
        right[i] = 0.f;
    }
    if (frames == 0) {
        return;
    }

    bool any_rendered = false;
    for (int vi = 0; vi < kSamplerMaxVoices; ++vi) {
        Voice& v = voices_[static_cast<std::size_t>(vi)];
        if (!v.active) {
            continue;
        }
        SampleData const& s = *v.sample;
        float const* d = s.samples.data();
        std::uint32_t const ch = s.channels;
        double const last = static_cast<double>(s.frames - 1);
        // End-of-sample ramp length in source frames (short samples: 1/4).
        double const end_len = static_cast<double>(
            s.frames / 4u < kEndRampFrames ? s.frames / 4u : kEndRampFrames);
        // A voice that deactivates mid-block may still have written samples
        // above, so the clip pass must run for this block.
        any_rendered = true;

        for (std::uint32_t i = 0; i < frames; ++i) {
            if (v.pos >= last) {  // sample finished
                v.active = false;
                break;
            }
            std::uint32_t const i0 = static_cast<std::uint32_t>(v.pos);
            float const frac = static_cast<float>(v.pos) -
                               static_cast<float>(i0);
            // Linear interpolation; i0 + 1 <= frames - 1 is guaranteed by
            // pos < last above.
            float l = 0.f;
            float r = 0.f;
            if (ch == 2) {
                std::uint32_t const b = i0 * 2u;
                float const l0 = d[b];
                float const r0 = d[b + 1];
                l = l0 + (d[b + 2] - l0) * frac;
                r = r0 + (d[b + 3] - r0) * frac;
            } else {
                float const x0 = d[i0];
                l = x0 + (d[i0 + 1] - x0) * frac;
                r = l;  // mono -> both channels
            }

            // End-of-sample ramp (click-free one-shot tail).
            double const to_end = last - v.pos;
            float end_fade = 1.f;
            if (to_end < end_len) {
                end_fade = static_cast<float>(to_end / end_len);
            }

            // Decay envelope + choke fade.
            v.env *= v.decay_factor;
            if (v.fade_step > 0.f) {
                v.fade -= v.fade_step;
                if (v.fade <= 0.f) {
                    v.fade = 0.f;
                    v.active = false;
                    break;
                }
            }
            float const g = v.gain * v.env * v.fade * end_fade;
            left[i] += l * g;
            right[i] += r * g;
            v.pos += static_cast<double>(v.rate);
            if (v.env < kEnvSilence) {
                v.active = false;
                break;
            }
        }
    }

    if (any_rendered) {
        // Master soft clip: keeps heavy polyphony inside (-1, 1); tanh(0)
        // is exactly 0 so silence stays exact.
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] = std::tanh(left[i]);
            right[i] = std::tanh(right[i]);
        }
    }
}

int Sampler::active_voices() const noexcept {
    int count = 0;
    for (auto const& v : voices_) {
        if (v.active) {
            ++count;
        }
    }
    return count;
}

bool Sampler::has_zone(int midi_note) const noexcept {
    if (midi_note < 0 || midi_note > 127) {
        return false;
    }
    return zones_[static_cast<std::size_t>(midi_note)] != nullptr;
}

}  // namespace addu94::audio
