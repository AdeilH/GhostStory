// addu94 — tests/test_coverage.cpp
// Spec §5 acceptance gate: every manifest control bound in ≥1 mode.
#include "test_framework.hpp"

#include "device/manifest.hpp"
#include "mapping/audit.hpp"
#include "mapping/mapping.hpp"

TEST(coverage, generic_inventory_has_zero_dead_controls) {
    auto const inv = addu94::device::default_inventory("generic-mpk");
    auto const dead = addu94::mapping::audit_coverage(inv);
    if (!dead.empty()) {
        std::string ids;
        for (auto const& d : dead) ids += d + " ";
        addu94::test::report_failure(__FILE__, __LINE__,
                                     "dead controls: " + ids);
    }
}

TEST(coverage, mk3_inventory_has_zero_dead_controls) {
    auto const inv = addu94::device::default_inventory("mpk-mini-mk3");
    auto const dead = addu94::mapping::audit_coverage(inv);
    CHECK(dead.empty());
}

TEST(coverage, every_binding_action_is_valid) {
    for (auto const& e : addu94::mapping::coverage_table()) {
        // Action::None is valid (direct device paths: keys, pedal, step grid).
        CHECK(static_cast<int>(e.action) <= 40);
    }
}

TEST_MAIN()
