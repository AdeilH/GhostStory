// addu94 — src/music/scales.hpp
//
// Scale tables (constexpr, evaluated at compile time — ADR 001 C++26
// constexpr usage) and scale-lock (spec §2.6): wrong notes snap to the
// nearest scale degree, never passed through.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace addu94::music {

struct Scale {
    std::string_view name;
    std::array<int, 12> semitones;  // intervals within the octave
    int count;                      // used entries
};

// Interval tables relative to the root. Used entries only; `count` bounds.
inline constexpr std::array<Scale, 8> kScales{{
    {"chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 12},
    {"major", {0, 2, 4, 5, 7, 9, 11, 0, 0, 0, 0, 0}, 7},
    {"minor", {0, 2, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0}, 7},
    {"dorian", {0, 2, 3, 5, 7, 9, 10, 0, 0, 0, 0, 0}, 7},
    {"phrygian", {0, 1, 3, 5, 7, 8, 10, 0, 0, 0, 0, 0}, 7},
    {"lydian", {0, 2, 4, 6, 7, 9, 11, 0, 0, 0, 0, 0}, 7},
    {"mixolydian", {0, 2, 4, 5, 7, 9, 10, 0, 0, 0, 0, 0}, 7},
    {"pentatonic-minor", {0, 3, 5, 7, 10, 0, 0, 0, 0, 0, 0, 0}, 5},
}};

[[nodiscard]] constexpr Scale const& scale_by_name(std::string_view n) noexcept {
    for (auto const& s : kScales) {
        if (s.name == n) return s;
    }
    return kScales[0];  // chromatic = identity
}

/// Is `note` (MIDI 0..127) in `scale` rooted at `root` (0..11)?
[[nodiscard]] constexpr bool in_scale(Scale const& s, int root, int note) noexcept {
    int const pc = ((note - root) % 12 + 12) % 12;
    for (int i = 0; i < s.count; ++i) {
        if (s.semitones[static_cast<std::size_t>(i)] == pc) return true;
    }
    return false;
}

/// Snap to the nearest scale tone. Ties snap upward (musically stable).
/// Octave-preserving: searches ±6 semitones max, then returns note.
[[nodiscard]] constexpr int snap_to_scale(Scale const& s, int root,
                                          int note) noexcept {
    if (in_scale(s, root, note)) return note;
    for (int d = 1; d <= 6; ++d) {
        if (in_scale(s, root, note + d) && note + d <= 127) return note + d;
        if (in_scale(s, root, note - d) && note - d >= 0) return note - d;
    }
    return note;  // chromatic fallback can't happen; safety net
}

}  // namespace addu94::music
