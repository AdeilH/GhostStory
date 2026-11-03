// addu94 — src/music/metronome.hpp
//
// Deterministic click generator (RT-safe): pure function of (sample rate,
// accent, phase). No state, no allocation — the audio callback asks the
// transport for click onsets in the buffer and renders them here.
#pragma once

#include <cmath>
#include <cstddef>

namespace addu94::music {

/// Render one metronome click starting at sample `offset` of the buffer.
/// Sound: short sine ping (accent 1900 Hz, beat 1270 Hz) with exp decay.
/// Deterministic: identical inputs → bit-identical output (golden-tested).
inline void render_click(float* buf, std::size_t frames, std::size_t offset,
                         bool accent, float gain, double sample_rate) noexcept {
    double const freq = accent ? 1900.0 : 1270.0;
    double const decay = 60.0;  // 1/e time in ms → tau
    std::size_t const len = static_cast<std::size_t>(sample_rate * 0.03) + 1;
    double const w = 2.0 * M_PI * freq / sample_rate;
    double const a = 1.0 / (decay * 0.001 * sample_rate);
    float const g = accent ? gain : gain * 0.6f;
    for (std::size_t i = 0; i < len && offset + i < frames; ++i) {
        double const t = static_cast<double>(i);
        double const env = std::exp(-t * a);
        // Slight click transient (2 samples) so timing is audible/visible.
        double const attack = t < 2.0 ? (t + 1.0) / 2.0 : 1.0;
        double const gd = static_cast<double>(g);  // avoid float→double promotion
        buf[offset + i] += static_cast<float>(gd * env * attack * std::sin(w * t));
    }
}

}  // namespace addu94::music
