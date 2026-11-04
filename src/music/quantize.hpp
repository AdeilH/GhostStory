// addu94 — src/music/quantize.hpp
//
// Quantizer (spec §2.5): snaps recorded MIDI event times to the grid.
// Pure functions over (tick, grid) — property-tested. Grids: off, 1/4,
// 1/8, 1/16 (default), 1/8T, 1/16T. Works in 96-PPQ ticks so math is
// integer-exact.
#pragma once

#include <cstdint>

#include "music/transport.hpp"  // kPPQ

namespace addu94::music {

enum class Grid : std::uint8_t {
    Off = 0,
    Quarter,       // 96 ticks
    Eighth,        // 48
    Sixteenth,     // 24
    EighthT,       // 32 (triplet eighths: 96 * 2/6)
    SixteenthT,    // 16
    ThirtySecond,  // 12 — added for the QuantizeGridSet knob (4-zone mapping
                   // off/1/8/1/16/1/32); integer-exact at 96 PPQ
};

/// Grid period in ticks at 96 PPQ.
[[nodiscard]] constexpr int grid_ticks(Grid g) noexcept {
    switch (g) {
    case Grid::Off: return 0;
    case Grid::Quarter: return kPPQ;
    case Grid::Eighth: return kPPQ / 2;
    case Grid::Sixteenth: return kPPQ / 4;
    case Grid::EighthT: return kPPQ / 3;      // 32: 3 triplet 8ths per beat
    case Grid::SixteenthT: return kPPQ / 6;   // 16
    case Grid::ThirtySecond: return kPPQ / 8; // 12
    }
    return 0;
}

/// Snap tick to the nearest grid line. Strength 0..100% (50% = halfway).
/// Negative ticks clamp to 0. Never changes tick when grid is Off.
[[nodiscard]] constexpr std::int64_t quantize_tick(Grid g, std::int64_t tick,
                                                   int strength_pct) noexcept {
    int const period = grid_ticks(g);
    if (period <= 0) return tick;
    if (tick < 0) tick = 0;
    std::int64_t const line = tick / period;       // grid line below
    std::int64_t const below = line * period;
    std::int64_t const above = (line + 1) * period;
    std::int64_t const target =
        (tick - below < above - tick) ? below : above;  // ties snap up
    // Apply strength toward the grid line.
    std::int64_t const delta = target - tick;
    std::int64_t const moved =
        delta * strength_pct / 100;
    std::int64_t const out = tick + moved;
    return out < 0 ? 0 : out;
}

[[nodiscard]] constexpr const char* to_string(Grid g) noexcept {
    switch (g) {
    case Grid::Off: return "off";
    case Grid::Quarter: return "1/4";
    case Grid::Eighth: return "1/8";
    case Grid::Sixteenth: return "1/16";
    case Grid::EighthT: return "1/8T";
    case Grid::SixteenthT: return "1/16T";
    case Grid::ThirtySecond: return "1/32";
    }
    return "?";
}

}  // namespace addu94::music
