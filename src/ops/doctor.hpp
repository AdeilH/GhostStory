// ghost_story — src/ops/doctor.hpp
//
// `ghost_story doctor` (spec §6.6): one-command self-diagnosis. Human text +
// doctor-report.json. Every check returns pass/warn/fail with evidence.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ghost_story::ops {

enum class CheckStatus : std::uint8_t { Pass, Warn, Fail };

struct Check {
    std::string name;
    CheckStatus status;
    std::string detail;
};

struct DoctorReport {
    std::vector<Check> checks;
    bool all_pass() const noexcept;
    std::string to_text() const;
    std::string to_json() const;
};

/// Run all checks. Non-destructive; safe anytime.
DoctorReport run_doctor();

/// `ghost_story repair` (spec §6.7): config schema migration, journal replay
/// validation, manifest re-probe. Returns per-action results.
struct RepairReport {
    std::vector<Check> actions;
    bool to_json_file(std::string const& path) const;
};
DoctorReport run_repair();

}  // namespace ghost_story::ops
