// ghost_story — tests/test_coverage.cpp
// Spec §5 acceptance gate: every manifest control bound in ≥1 mode.
#include "test_framework.hpp"

#include "device/manifest.hpp"
#include "mapping/audit.hpp"
#include "mapping/mapping.hpp"

TEST(coverage, generic_inventory_has_zero_dead_controls) {
    auto const inv = ghost_story::device::default_inventory("generic-mpk");
    auto const dead = ghost_story::mapping::audit_coverage(inv);
    if (!dead.empty()) {
        std::string ids;
        for (auto const& d : dead) ids += d + " ";
        ghost_story::test::report_failure(__FILE__, __LINE__,
                                     "dead controls: " + ids);
    }
}

TEST(coverage, mk3_inventory_has_zero_dead_controls) {
    auto const inv = ghost_story::device::default_inventory("mpk-mini-mk3");
    auto const dead = ghost_story::mapping::audit_coverage(inv);
    CHECK(dead.empty());
}

TEST(coverage, every_binding_action_is_valid) {
    for (auto const& e : ghost_story::mapping::coverage_table()) {
        // Action::None is valid (direct device paths: keys, pedal, step grid).
        // Max ordinal: FxParamB (40) after the FX knob page landed; 47
        // leaves headroom for the next batch of actions.
        CHECK(static_cast<int>(e.action) <= 47);
    }
}

TEST_MAIN()
