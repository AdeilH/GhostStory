// addu94 — src/music/velocity.hpp
//
// Velocity curves (spec §4): mini keys under-report low velocities, so the
// default curve is NOT linear. Curves are pure functions — RT-safe, no
// state — and user-tunable per track (spec §5 PLAY mode).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace addu94::music {

enum class VelocityCurve : std::uint8_t {
    Linear = 0,
    Log,        // more resolution at the bottom (default: mini keys)
    Exp,        // more resolution at the top
    HardFloor,  // lifts weak strikes: out = max(v, floor) with floor=32
};

/// Map input velocity 1..127 → output 1..127. 0 (note-off) passes through.
[[nodiscard]] inline int apply_velocity_curve(VelocityCurve c, int v) noexcept {
    if (v <= 0) return 0;
    if (v > 127) v = 127;
    switch (c) {
    case VelocityCurve::Linear:
        break;
    case VelocityCurve::Log: {
        // out = 127 * (v/127)^(1/2) — compresses loud, expands soft.
        double const x = static_cast<double>(v) / 127.0;
        return std::max(1, static_cast<int>(127.0 * std::sqrt(x) + 0.5));
    }
    case VelocityCurve::Exp: {
        double const x = static_cast<double>(v) / 127.0;
        return std::max(1, static_cast<int>(127.0 * x * x + 0.5));
    }
    case VelocityCurve::HardFloor:
        return std::max(v, 32);
    }
    return v;
}

[[nodiscard]] constexpr const char* to_string(VelocityCurve c) noexcept {
    switch (c) {
    case VelocityCurve::Linear: return "linear";
    case VelocityCurve::Log: return "log";
    case VelocityCurve::Exp: return "exp";
    case VelocityCurve::HardFloor: return "hard-floor";
    }
    return "?";
}

}  // namespace addu94::music
