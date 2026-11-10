// ghost_story — src/project/smf.hpp
//
// Standard MIDI File (type 1) export (spec §2.7): header + tempo track +
// one track per clip. 96 PPQ ticks map 1:1 to our internal grid. Big-endian
// VLQ lengths per the SMF spec. Deterministic output (golden-tested).
#pragma once

#include "music/clip.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ghost_story::project {

/// Export one clip as an SMF type-1 byte buffer.
/// `tempo_bpm` writes a single tempo event on track 0.
[[nodiscard]] std::vector<std::uint8_t> export_smf_type1(
    music::Clip const& clip, double tempo_bpm, int meter_num, int meter_den);

/// Convenience: write to file (atomic replace).
bool write_smf_file(std::string const& path,
                    std::vector<std::uint8_t> const& bytes);

}  // namespace ghost_story::project
