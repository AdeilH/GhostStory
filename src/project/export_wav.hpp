// ghost_story — src/project/export_wav.hpp
//
// WAV export of a project's tracks (spec §2.7 / M6): renders every track
// offline (audio::render_track) and writes one 32-bit float stereo WAV
// per stem plus the master mix (audio::mix_down of all stems).
//
// The writer here is a private, minimal implementation inside
// export_wav.cpp on purpose: it must not depend on the concurrently
// developed src/audio/wav_file.* — the integrator unifies them later.
//
// Naming: stems are <output_dir>/track_1.wav, track_2.wav, ... (1-based,
// one per ExportSpec::tracks entry) and the mix is <output_dir>/master.wav.
#pragma once

#include "audio/render_engine.hpp"

#include <string>
#include <vector>

namespace ghost_story::project {

struct ExportSpec {
    std::string output_dir;                    // stems written here
    std::vector<audio::RenderRequest> tracks;  // one per track
    int loop_count{1};
};

/// Render + write all stems and the master mix. Returns false and fills
/// `error` on any I/O failure (directory creation, file write). The
/// master is the exact mix_down sum of the stems (M6 gate: within 1e-6).
bool export_wav_stems(ExportSpec const& spec, std::string& error);

}  // namespace ghost_story::project
