// addu94 — src/project/arrangement.hpp
//
// Arrangement (spec §2.4): pattern blocks placed on the song timeline.
// A Block pins a clip to (track, start_bar) with the clip's loop length;
// song sections (intro/verse/chorus) are named bar ranges. Pure model +
// helpers — the UI paints it, the renderer walks it.
#pragma once

#include "project/document.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace addu94::project {

struct Block {
    int track{0};
    int clip{0};        // index into Track::clips
    int start_bar{0};
    int length_bars{4}; // copied from clip at place time; user-stretchable
};

/// Blocks for the timeline, attached to the project (serialized with it).
using Timeline = std::vector<Block>;

/// Bars a song spans (0 for empty).
[[nodiscard]] inline int song_length_bars(Timeline const& t) noexcept {
    int end = 0;
    for (auto const& b : t) end = std::max(end, b.start_bar + b.length_bars);
    return end;
}

/// Clips sounding at `bar` on `track` (overlaps allowed — layered loops).
[[nodiscard]] inline std::vector<Block> blocks_at(Timeline const& t, int track,
                                                  int bar) {
    std::vector<Block> out;
    for (auto const& b : t) {
        if (b.track == track && bar >= b.start_bar &&
            bar < b.start_bar + b.length_bars) {
            out.push_back(b);
        }
    }
    return out;
}

/// Section at `bar` (last matching range wins), nullptr when none.
[[nodiscard]] inline SongSection const* section_at(
    std::vector<SongSection> const& sections, int bar) {
    SongSection const* best = nullptr;
    for (auto const& s : sections) {
        if (bar >= s.start_bar && bar < s.start_bar + s.length_bars) {
            best = &s;
        }
    }
    return best;
}

}  // namespace addu94::project
