// addu94 — src/mapping/audit.hpp
//
// Control coverage audit (spec §5): every physical control on the detected
// device manifest must be bound in at least one mode. Runs as a CI test —
// a new manifest control without a binding FAILS the build.
#pragma once

#include "device/manifest.hpp"
#include "mapping/actions.hpp"
#include "mapping/mapping.hpp"

#include <string>
#include <vector>

namespace addu94::mapping {

struct CoverageEntry {
    std::string control_id;   // matches manifest Control::id
    Mode mode;                // mode in which the binding lives
    Action action;            // what it does there
};

/// The complete binding table. Extend when a control gains a new binding.
[[nodiscard]] std::vector<CoverageEntry> const& coverage_table();

/// Audit a manifest: returns the ids of controls with NO binding in any
/// mode. Empty = pass (zero dead controls).
[[nodiscard]] std::vector<std::string> audit_coverage(
    std::vector<device::Control> const& controls);

}  // namespace addu94::mapping
