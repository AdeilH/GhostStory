// ghost_story — src/ops/bench.cpp
#include "ops/bench.hpp"

#include "audio/engine.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace ghost_story::ops {

namespace {

long current_rss_kb() {
    std::ifstream f("/proc/self/statm");
    long total = 0, resident = 0;
    if (f.good()) {
        f >> total >> resident;
        return resident * (long(sysconf(_SC_PAGESIZE)) / 1024);
    }
    return 0;
}

}  // namespace

BenchReport run_bench(int seconds, std::string const& report_path,
                      std::string const& trace_path) {
    BenchReport rep;
    audio::AudioEngine engine;
    audio::BackendConfig cfg;
    std::string reason;
    if (!engine.start(cfg, reason)) {
        log::error("bench", "cannot start audio: {}", reason);
        rep.gate_pass = false;
        return rep;
    }
    auto st = engine.stats();
    rep.backend = engine.backend_name();
    rep.sample_rate = st.sample_rate;
    rep.buffer_size = st.buffer_size;

    engine.bench_start_record();
    auto const t0 = std::chrono::steady_clock::now();
    std::vector<std::uint32_t> durations;
    std::vector<audio::AudioEngine::TimingSample> samples;
    durations.reserve(static_cast<std::size_t>(seconds) *
                      st.sample_rate / st.buffer_size + 16);
    audio::AudioEngine::TimingSample scratch[512];
    while (std::chrono::duration<double>(
               std::chrono::steady_clock::now() - t0)
               .count() < static_cast<double>(seconds)) {
        std::size_t const n = engine.bench_drain(scratch, 512);
        for (std::size_t i = 0; i < n; ++i) {
            durations.push_back(scratch[i].dur_us);
            samples.push_back(scratch[i]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    engine.bench_stop_record();

    rep.duration_s = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    st = engine.stats();
    rep.callbacks = st.callbacks;
    rep.xruns = st.xruns;
    rep.rss_kb = current_rss_kb();
    engine.stop();

    std::sort(durations.begin(), durations.end());
    if (!durations.empty()) {
        auto const at = [&](double p) {
            auto const idx = static_cast<std::size_t>(
                std::ceil(p * static_cast<double>(durations.size() - 1)));
            return static_cast<double>(durations[idx]);
        };
        rep.p50_us = at(0.50);
        rep.p95_us = at(0.95);
        rep.p99_us = at(0.99);
        rep.max_us = durations.back();
    }
    double const buffer_us =
        static_cast<double>(rep.buffer_size) * 1e6 /
        static_cast<double>(rep.sample_rate > 0 ? rep.sample_rate : 48000);
    rep.p99_budget_pct = buffer_us > 0 ? rep.p99_us / buffer_us * 100.0 : 0.0;
    rep.max_budget_pct = buffer_us > 0 ? static_cast<double>(rep.max_us) /
                                             buffer_us * 100.0
                                       : 0.0;
    rep.gate_pass = rep.p99_budget_pct < 30.0 && rep.xruns == 0;

    // Chrome-trace output (spec §7.6) when requested: one X event per
    // callback so the JSON loads directly into chrome://tracing / Perfetto.
    if (!trace_path.empty()) {
        nlohmann::json ev = nlohmann::json::array();
        for (auto const& s : samples) {
            ev.push_back({{"ph", "X"},
                          {"name", "audio-callback"},
                          {"tid", 0},
                          {"pid", 1},
                          {"ts", s.start_us},
                          {"dur", s.dur_us}});
        }
        std::ofstream tf(trace_path, std::ios::trunc);
        tf << nlohmann::json{{"traceEvents", ev},
                             {"displayTimeUnit", "ms"}}
                  .dump();
    }

    nlohmann::json j = {
        {"schema", 1},
        {"duration_s", rep.duration_s},
        {"backend", rep.backend},
        {"sample_rate", rep.sample_rate},
        {"buffer_size", rep.buffer_size},
        {"callbacks", rep.callbacks},
        {"xruns", rep.xruns},
        {"callback_us", {{"p50", rep.p50_us},
                         {"p95", rep.p95_us},
                         {"p99", rep.p99_us},
                         {"max", rep.max_us}}},
        {"budget_pct", {{"p99", rep.p99_budget_pct},
                        {"max", rep.max_budget_pct}}},
        {"rss_kb", rep.rss_kb},
        {"gate_pass", rep.gate_pass},
    };
    std::ofstream f(report_path, std::ios::trunc);
    f << j.dump(2);
    log::info("bench", "bench complete: p99={}us ({:.1f}% of buffer), xruns={}, gate={}",
              static_cast<int>(rep.p99_us), rep.p99_budget_pct, rep.xruns,
              rep.gate_pass ? "PASS" : "FAIL");
    return rep;
}

}  // namespace ghost_story::ops
