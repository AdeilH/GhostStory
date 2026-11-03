// addu94 — src/app/main.cpp
//
// Single binary, subcommand CLI (headless-first). The GUI (M4+) is entered
// via the default command when a display is available; all subcommands work
// headless so CI never needs one.
#include "app/play_engine.hpp"
#include "device/probe.hpp"
#include "log/logger.hpp"
#include "ops/bench.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>

namespace {

constexpr std::string_view kVersion = "0.1.0-dev";

void print_usage() {
    std::fputs(
        "addu94 — self-healing song builder for the Akai MPK Mini (Linux)\n"
        "\n"
        "Usage: addu94 <command>\n"
        "\n"
        "Commands:\n"
        "  doctor        Run self-diagnosis and write doctor-report.json\n"
        "  repair        Verify/rebuild config, journal, samples, manifest\n"
        "  bench         Run benchmark suite, write bench-report.json\n"
        "  probe         Probe connected MIDI devices, write device manifest\n"
        "  play          Launch the app (GUI when a display is present)\n"
        "  version       Print version\n"
        "\n"
        "See docs/ for the full manual. https://addu94.local (offline-first)\n",
        stdout);
}

int cmd_version() {
    std::puts(std::format("addu94 {}", kVersion).c_str());
    return 0;
}

int cmd_play(int auto_stop_seconds) {
    addu94::log::Logger::init("", addu94::log::Level::Info);
    addu94::app::PlayEngine engine;
    std::string error;
    if (!engine.start(error)) {
        std::fprintf(stderr, "addu94: failed to start: %s\n", error.c_str());
        addu94::log::Logger::shutdown();
        return 1;
    }
    std::printf(
        "addu94 is live. Play your MPK Mini — keys make sound, pads fire "
        "chord slots, knobs mix.\n");
    if (auto_stop_seconds > 0) {
        std::printf("(auto-stop after %d s)\n", auto_stop_seconds);
        std::this_thread::sleep_for(std::chrono::seconds(auto_stop_seconds));
    } else {
        std::printf("(Ctrl-C to quit)\n");
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
    std::printf("voices active: %d, audio running: %d\n", engine.active_voices(),
                engine.audio_running() ? 1 : 0);
    engine.stop();
    addu94::log::Logger::shutdown();
    return 0;
}

int cmd_doctor() {
    // M7 delivers the full doctor; M0 ships the skeleton with honest
    // "not yet implemented" statuses so the command surface exists.
    using addu94::log::Level;
    addu94::log::Logger::init("", Level::Info);
    addu94::log::info("doctor", "addu94 doctor {} — skeleton (full checks land in M7)", kVersion);
    std::puts("addu94 doctor (skeleton)\n"
              "  [ ] audio graph .............. (M7)\n"
              "  [ ] midi ports ............... (M7)\n"
              "  [ ] realtime permissions ..... (M7)\n"
              "  [ ] config integrity ......... (M7)\n"
              "  [ ] disk space ............... (M7)");
    addu94::log::Logger::shutdown();
    return 0;
}

int cmd_probe(bool capture) {
    addu94::device::Probe probe;
    int const seconds = capture ? 60 : 0;
    std::printf("addu94 probe: scanning ALSA MIDI ports%s...\n",
                capture ? " (60 s capture)" : "");
    auto result = probe.run(seconds);
    std::puts(result.summary.c_str());
    if (!result.manifest.model.empty()) {
        // Auto-save manifest so the mapping layer is manifest-driven from
        // the very first run (spec §1).
        auto const path = addu94::device::manifest_path();
        if (addu94::device::save_manifest(result.manifest, path)) {
            std::printf("manifest written: %s\n", path.c_str());
        } else {
            std::fprintf(stderr, "failed to write manifest: %s\n",
                         path.c_str());
            return 1;
        }
    }
    if (capture && !probe.captured().empty()) {
        std::printf("captured %zu raw message(s); see docs/sysex.md\n",
                    probe.captured().size());
    }
    return result.device_found ? 0 : 3;
}

int cmd_bench(int seconds) {
    addu94::log::Logger::init("", addu94::log::Level::Info);
    std::printf("addu94 bench: %d s audio callback load test...\n", seconds);
    auto const rep = addu94::ops::run_bench(seconds, "bench-report.json");
    std::printf(
        "backend=%s %uHz %u frames | callbacks=%llu xruns=%llu\n"
        "callback us: p50=%.0f p95=%.0f p99=%.0f max=%u (p99 = %.1f%% of "
        "buffer budget, gate < 30%%)\n"
        "rss=%ld kB | report: bench-report.json | gate: %s\n",
        rep.backend.c_str(), rep.sample_rate, rep.buffer_size,
        static_cast<unsigned long long>(rep.callbacks),
        static_cast<unsigned long long>(rep.xruns), rep.p50_us, rep.p95_us,
        rep.p99_us, rep.max_us, rep.p99_budget_pct, rep.rss_kb,
        rep.gate_pass ? "PASS" : "FAIL");
    addu94::log::Logger::shutdown();
    return rep.gate_pass ? 0 : 1;
}

}  // namespace

int main(int argc, char const** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    std::string_view const cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "version" || cmd == "--version") return cmd_version();
    if (cmd == "doctor") return cmd_doctor();
    if (cmd == "probe") {
        bool capture = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--capture") capture = true;
        }
        return cmd_probe(capture);
    }
    if (cmd == "bench") {
        int seconds = 60;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--seconds") {
                seconds = std::atoi(argv[i + 1]);
            }
        }
        return cmd_bench(seconds);
    }

    // Milestone-gated commands: exist as surface, fail honestly until their
    // milestone lands.
    if (cmd == "play") {
        int seconds = 0;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--seconds") {
                seconds = std::atoi(argv[i + 1]);
            }
        }
        return cmd_play(seconds);
    }
    if (cmd == "repair") {
        std::fprintf(stderr,
                     "addu94: command '%s' is not implemented yet "
                     "(planned milestone; see ROADMAP.md)\n",
                     std::string{cmd}.c_str());
        return 2;
    }
    std::fprintf(stderr, "addu94: unknown command '%s'\n\n",
                 std::string{cmd}.c_str());
    print_usage();
    return 2;
}
