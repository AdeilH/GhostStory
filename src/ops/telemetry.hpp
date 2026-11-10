// ghost_story — src/ops/telemetry.hpp
//
// Local-only, opt-out telemetry (spec §7.1): JSONL session events at
// ~/.local/share/ghost_story/telemetry/sessions.jsonl. Deterministic event
// schemas; nothing leaves the machine unless the user exports a report.
#pragma once

#include <cstdint>
#include <string>

namespace ghost_story::ops {

struct SessionStats {
    std::uint64_t xruns{0};
    std::uint64_t max_callback_us{0};
    std::uint64_t midi_events{0};
    std::uint64_t crashes_recovered{0};
    double duration_s{0};
};

class Telemetry {
public:
    /// enabled=false disables ALL writes (spec: opt-out).
    static void init(bool enabled);
    static void shutdown() noexcept;

    static void session_start();
    static void session_end(SessionStats const& stats);
    static void event(std::string const& kind, std::string const& detail);

    /// Aggregate summary for the gardener loop: returns JSONL as string.
    [[nodiscard]] static std::string load_recent(int max_lines);
};

}  // namespace ghost_story::ops
