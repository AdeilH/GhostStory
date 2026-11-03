// addu94 — src/audio/render_engine.hpp
//
// Offline (non-RT) renderer for export (spec §2.7): renders one track's
// clip loop N times through Synth + FxChain, sample-exactly, and mixes
// stems to a master. Unlike the live engine there is no RT constraint
// here — allocation is fine — but the output must be DETERMINISTIC: the
// same RenderRequest always produces bit-identical samples (M6 gate).
//
// Event timing: tick -> samples = tick * (60/bpm) / PPQ * sr, evaluated
// per event (music/transport.hpp is the RT reference; offline we compute
// positions directly). Note on/off pairs are triggered into the synth at
// their exact sample offsets by splitting each render block at event
// boundaries.
#pragma once

#include "audio/fx.hpp"
#include "audio/synth.hpp"
#include "music/clip.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace addu94::audio {

struct RenderRequest {
    music::Clip const* clip{nullptr};  // clip to render (loop, once through)
    SynthParams synth_params;          // per-track instrument (M6: synth)
    FxParams fx;
    float track_volume{1.0f};
    double tempo_bpm{120.0};
    int meter_num{4};   // informational: tick math depends only on bpm/PPQ
    int meter_den{4};
    double sample_rate{48000.0};
};

struct RenderResult {
    std::vector<float> left;
    std::vector<float> right;
};

/// Render one track's clip looped `loop_count` times through
/// synth -> fx -> track_volume. Returns empty vectors when the request
/// has no clip, a non-positive clip length or loop_count <= 0.
RenderResult render_track(RenderRequest const& req, int loop_count);

/// Master = sum of stems (pre-clip master bus, no limiting). Shorter
/// stems are padded with silence. The M6 tolerance gate feeds on this:
/// master must equal the element-wise sum of the stems within 1e-6.
RenderResult mix_down(std::vector<RenderResult> const& stems);

}  // namespace addu94::audio
