// ghost_story — src/app/main.cpp
//
// Single binary, subcommand CLI (headless-first). The GUI (M4+) is entered
// via the default command when a display is available; all subcommands work
// headless so CI never needs one.
#include "app/calibrate.hpp"
#include "app/play_engine.hpp"
#include "project/smf.hpp"
#include "project/export_wav.hpp"
#include "project/journal.hpp"
#include "device/probe.hpp"
#include "log/logger.hpp"
#include "ops/bench.hpp"
#include "ops/crash_handler.hpp"
#include "ops/doctor.hpp"
#include "ops/telemetry.hpp"

#include <fstream>

#if defined(GHOST_STORY_UI_ENABLED)
#include "ui/app_gui.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>

namespace {

constexpr std::string_view kVersion = "0.1.0";

// Log-file resolution for interactive runs: --log-file PATH wins, then
// GHOST_STORY_LOG_FILE ("0" disables, any other value is a path), then the
// default XDG data path. Empty result = stderr only (logger stays file-less).
std::string resolve_log_file(char const* flag_path) {
    if (flag_path != nullptr) return std::string{flag_path};
    char const* env = std::getenv("GHOST_STORY_LOG_FILE");
    if (env != nullptr) {
        if (std::string_view{env} == "0") return {};
        return std::string{env};
    }
    return ghost_story::log::default_log_path();
}

void print_usage() {
    std::fputs(
        "Ghost Story — self-healing song builder for the Akai MPK Mini (Linux)\n"
        "\n"
        "Usage: ghost_story <command>\n"
        "\n"
        "Commands:\n"
        "  doctor        Run self-diagnosis and write doctor-report.json\n"
        "  repair        Verify/rebuild config, journal, samples, manifest\n"
        "  bench         Run benchmark suite, write bench-report.json\n"
        "  probe         Probe connected MIDI devices, write device manifest\n"
        "  calibrate     Map the 8 hardware knobs to CCs (writes profile.toml)\n"
        "  play          Launch the app (native GUI; --headless keeps the\n"
        "                console loop, --seconds N auto-stops after N s;\n"
        "                --log-file PATH overrides the JSONL log path,\n"
        "                --no-log-file keeps stderr-only logging)\n"
        "  version       Print version\n"
        "\n"
        "See docs/ for the full manual. https://ghost-story.local (offline-first)\n",
        stdout);
}

int cmd_version() {
    std::puts(std::format("ghost_story {}", kVersion).c_str());
    return 0;
}

int cmd_play(int auto_stop_seconds, bool headless, char const* project_dir,
             char const* log_file, bool no_log_file) {
    std::string const log_path =
        no_log_file ? std::string{} : resolve_log_file(log_file);
    ghost_story::log::Logger::init(log_path, ghost_story::log::Level::Info);
    if (!log_path.empty()) {
        ghost_story::log::info("log", "writing log file: {}", log_path);
    }
    ghost_story::app::PlayEngine engine;
    std::string error;
    if (project_dir != nullptr &&
        !engine.attach_project(project_dir, error)) {
        std::fprintf(stderr, "ghost_story: project load failed: %s\n",
                     error.c_str());
        ghost_story::log::Logger::shutdown();
        return 1;
    }
    if (!engine.start(error)) {
        std::fprintf(stderr, "ghost_story: failed to start: %s\n", error.c_str());
        ghost_story::log::Logger::shutdown();
        return 1;
    }
    ghost_story::ops::Telemetry::session_start();
    std::printf(
        "Ghost Story is live. Play your MPK Mini — keys play the selected "
        "track, all 16 pads fire the kit in PLAY, KIT mode pads pick the "
        "pack, knobs mix.\n");
    int rc = 0;
#if defined(GHOST_STORY_UI_ENABLED)
    if (!headless) {
        // Native GUI (spec §9): owns the app-event drain (30 Hz timer) and
        // the transport/mapping control plane. auto_stop_seconds > 0 quits
        // the event loop after that time (CI smoke runs).
        if (auto_stop_seconds > 0) {
            std::printf("(auto-stop after %d s)\n", auto_stop_seconds);
        }
        rc = ghost_story::ui::run_gui(engine, auto_stop_seconds);
    } else
#endif
    {
        // Headless path (unchanged): sleep loop, no display, no Qt.
        if (auto_stop_seconds > 0) {
            std::printf("(auto-stop after %d s)\n", auto_stop_seconds);
            std::this_thread::sleep_for(std::chrono::seconds(auto_stop_seconds));
        } else {
            std::printf("(Ctrl-C to quit)\n");
            for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    std::printf("audio running: %d\n", engine.audio_running() ? 1 : 0);
    engine.stop();
    // Clean exit: retire this run's still-empty crash marker and clear the
    // safe-mode flag. A run killed mid-session never gets here, which is
    // what makes the flag (and any surviving report) mean something — the
    // zero-byte "crash" pile-up happened because this hook never ran.
    ghost_story::ops::CrashHandler::mark_clean_shutdown();
    // Queue-pressure visibility for ops: a non-zero drop count means the
    // RT-safe queue overflowed — warn so the alert query ("dropped")
    // catches it; clean runs stay at debug (no alert noise).
    if (auto const dropped = ghost_story::log::Logger::dropped_entries();
        dropped > 0) {
        ghost_story::log::warn("log", "shutdown: {} log entries dropped "
                                      "(RT queue overflow)",
                               dropped);
    } else {
        ghost_story::log::debug("log", "shutdown: clean, no entries dropped");
    }
    ghost_story::log::Logger::shutdown();
    return rc;
}

int cmd_doctor(bool repair) {
    ghost_story::log::Logger::init("", ghost_story::log::Level::Error);
    auto const rep = repair ? ghost_story::ops::run_repair()
                            : ghost_story::ops::run_doctor();
    std::fputs(std::format("ghost_story {} report\n", repair ? "repair" : "doctor")
                   .c_str(),
               stdout);
    std::fputs(rep.to_text().c_str(), stdout);
    std::ofstream f(repair ? "repair-report.json" : "doctor-report.json");
    f << rep.to_json();
    std::printf("report: %s\n", repair ? "repair-report.json"
                                        : "doctor-report.json");
    ghost_story::log::Logger::shutdown();
    return rep.all_pass() ? 0 : 1;
}

int cmd_probe(bool capture) {
    ghost_story::device::Probe probe;
    int const seconds = capture ? 60 : 0;
    std::printf("ghost_story probe: scanning ALSA MIDI ports%s...\n",
                capture ? " (60 s capture)" : "");
    auto result = probe.run(seconds);
    std::puts(result.summary.c_str());
    if (!result.manifest.model.empty()) {
        // Auto-save manifest so the mapping layer is manifest-driven from
        // the very first run (spec §1).
        auto const path = ghost_story::device::manifest_path();
        if (ghost_story::device::save_manifest(result.manifest, path)) {
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

int cmd_bench(int seconds, bool trace) {
    ghost_story::log::Logger::init("", ghost_story::log::Level::Info);
    std::printf("ghost_story bench: %d s audio callback load test...\n", seconds);
    auto const rep = ghost_story::ops::run_bench(
        seconds, "bench-report.json",
        trace ? "callback-trace.json" : std::string{});
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
    ghost_story::log::Logger::shutdown();
    return rep.gate_pass ? 0 : 1;
}

}  // namespace

int main(int argc, char const** argv) {
    // Crash handling + safe mode (spec §6.4/§6.5): install first; if the
    // previous run crashed, surface the safe-mode offer.
    bool const last_run_crashed = ghost_story::ops::CrashHandler::install();
    bool const telemetry_opt_out =
        std::getenv("GHOST_STORY_TELEMETRY") != nullptr &&
        std::string_view{std::getenv("GHOST_STORY_TELEMETRY")} == "0";
    ghost_story::ops::Telemetry::init(!telemetry_opt_out);
    ghost_story::ops::Telemetry::event("launch", last_run_crashed ? "after-crash" : "");
    if (last_run_crashed) {
        std::fprintf(stderr,
                     "ghost_story: previous run crashed. Reports in %s.\n"
                     "        Run 'ghost_story doctor' for diagnostics.\n",
                     ghost_story::ops::CrashHandler::crash_dir().c_str());
    }
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
    if (cmd == "doctor" || cmd == "repair") {
        return cmd_doctor(cmd == "repair");
    }
    if (cmd == "probe") {
        bool capture = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--capture") capture = true;
        }
        return cmd_probe(capture);
    }
    if (cmd == "bench") {
        int seconds = 60;
        bool trace = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--seconds" && i + 1 < argc) {
                seconds = std::atoi(argv[i + 1]);
            } else if (std::string_view{argv[i]} == "--trace") {
                trace = true;
            }
        }
        return cmd_bench(seconds, trace);
    }

    // Milestone-gated commands: exist as surface, fail honestly until their
    // milestone lands.
    if (cmd == "play") {
        int seconds = 0;
        bool headless = false;
        bool no_log_file = false;
        char const* log_file = nullptr;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view{argv[i]} == "--seconds" && i + 1 < argc) {
                seconds = std::atoi(argv[i + 1]);
            } else if (std::string_view{argv[i]} == "--headless") {
                headless = true;
            } else if (std::string_view{argv[i]} == "--no-log-file") {
                no_log_file = true;
            } else if (std::string_view{argv[i]} == "--log-file" &&
                       i + 1 < argc) {
                log_file = argv[i + 1];
            }
        }
        char const* project_dir = nullptr;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--project-dir") {
                project_dir = argv[i + 1];
            }
        }
        return cmd_play(seconds, headless, project_dir, log_file, no_log_file);
    }
    if (cmd == "selftest-record") {
        // Kill -9 gate driver (spec §6.3): records synthetic notes through
        // the real event queue → recorder → journal pipeline, forever (or
        // --seconds N) until the test kills it. Not a user command.
        char const* project_dir = nullptr;
        int interval_ms = 50;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--project-dir") {
                project_dir = argv[i + 1];
            } else if (std::string_view{argv[i]} == "--interval-ms") {
                interval_ms = std::atoi(argv[i + 1]);
            }
        }
        if (project_dir == nullptr) {
            std::fprintf(stderr, "usage: selftest-record --project-dir DIR\n");
            return 2;
        }
        // File logging also on for the gate driver: after a SIGKILL the
        // surviving app.jsonl is the post-mortem record of the run.
        ghost_story::log::Logger::init(resolve_log_file(nullptr),
                                       ghost_story::log::Level::Error);
        ghost_story::app::PlayEngine engine;
        std::string error;
        if (!engine.attach_project(project_dir, error)) {
            std::fprintf(stderr, "ghost_story: %s\n", error.c_str());
            return 1;
        }
        // Tracks are dynamic and start empty — the selftest drums on
        // channel 10, so give it a Drums track.
        if (!engine.drums_present()) {
            if (!engine.add_track(-1, error)) {
                std::fprintf(stderr, "ghost_story: %s\n", error.c_str());
                return 1;
            }
        }
        engine.command({ghost_story::mapping::Action::TransportRecord, 0, 0});
        int n = 0;
        for (;;) {
            bool const on = (n % 2) == 0;
            int const note = 36 + ((n / 2) % 8);  // pair on/off of SAME note
            engine.test_feed_note(9, note, 100, on);
            std::vector<ghost_story::mapping::Command> const drained =
                engine.drain_app_events();
            (void)drained;
            ++n;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(interval_ms));
        }
    }
    if (cmd == "calibrate") {
        char const* out = nullptr;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--out") out = argv[i + 1];
        }
        return ghost_story::app::cmd_calibrate(out);
    }
    if (cmd == "export") {
        char const* project_dir = nullptr;
        char const* out = nullptr;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view{argv[i]} == "--project-dir") {
                project_dir = argv[i + 1];
            } else if (std::string_view{argv[i]} == "--smf") {
                out = argv[i + 1];
            } else if (std::string_view{argv[i]} == "--wav-dir") {
                out = argv[i + 1];
            }
        }
        if (project_dir == nullptr || out == nullptr) {
            std::fprintf(stderr,
                         "usage: ghost_story export --project-dir DIR "
                         "(--smf OUT.mid | --wav-dir DIR)\n");
            return 2;
        }
        ghost_story::project::Project p;
        std::string error;
        if (!ghost_story::project::load_project(project_dir, p, error)) {
            std::fprintf(stderr, "ghost_story: %s\n", error.c_str());
            return 1;
        }
        if (p.tracks.empty() || p.tracks[0].clips.empty()) {
            std::fprintf(stderr, "ghost_story: nothing to export (no clips)\n");
            return 3;
        }
        if (std::string_view{out}.ends_with(".mid")) {
            auto const bytes = ghost_story::project::export_smf_type1(
                p.tracks[0].clips[0], p.tempo_bpm, p.meter_num, p.meter_den);
            if (!ghost_story::project::write_smf_file(out, bytes)) {
                std::fprintf(stderr, "ghost_story: cannot write %s\n", out);
                return 1;
            }
            std::printf("wrote %s (%zu bytes)\n", out, bytes.size());
            return 0;
        }
        if (std::string_view{out} == "--wav-dir") {
            std::fprintf(stderr, "ghost_story: use --wav-dir DIR as the value\n");
            return 2;
        }
        // WAV stems + master (spec §2.7): render the recorded clip through
        // synth + drums + FX offline.
        ghost_story::project::ExportSpec spec;
        spec.output_dir = out;
        ghost_story::audio::RenderRequest req;
        req.clip = &p.tracks[0].clips[0];
        req.tempo_bpm = p.tempo_bpm;
        req.sample_rate = 48000.0;
        spec.tracks.push_back(req);
        std::string err;
        if (!ghost_story::project::export_wav_stems(spec, err)) {
            std::fprintf(stderr, "ghost_story: wav export failed: %s\n",
                         err.c_str());
            return 1;
        }
        std::printf("wrote stems + master to %s\n", out);
        return 0;
    }

    std::fprintf(stderr, "ghost_story: unknown command '%s'\n\n",
                 std::string{cmd}.c_str());
    print_usage();
    return 2;
}
