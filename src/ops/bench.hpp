// addu94 — src/ops/bench.hpp
//
// Benchmark suite (spec §7.2): audio callback headroom percentiles,
// xrun count, memory footprint. Writes bench-report.json. Deterministic
// report format so CI can diff regressions.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace addu94::ops {

struct BenchReport {
    double duration_s{0};
    std::string backend;
    std::uint32_t sample_rate{0};
    std::uint32_t buffer_size{0};
    std::uint64_t callbacks{0};
    std::uint64_t xruns{0};
    // Callback duration percentiles in microseconds.
    double p50_us{0}, p95_us{0}, p99_us{0};
    std::uint32_t max_us{0};
    // Budget: p99 must be < 30% of buffer duration (spec M2 gate).
    double p99_budget_pct{0};  // p99 as percent of buffer duration
    double max_budget_pct{0};
    bool gate_pass{false};
    long rss_kb{0};
};

/// Run the bench for `seconds` (60 default; the M2 gate runs 300).
/// Writes bench-report.json to `report_path`; when `trace_path` is
/// non-empty, also writes a Chrome-trace-format JSON of callback timings
/// (spec §7.6, `--trace`). Returns the report.
BenchReport run_bench(int seconds, std::string const& report_path,
                      std::string const& trace_path = {});

}  // namespace addu94::ops
