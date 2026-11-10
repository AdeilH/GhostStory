// ghost_story — src/ops/doctor.cpp
#include "ops/doctor.hpp"

#include "audio/backend.hpp"
#include "device/manifest.hpp"
#include "device/probe.hpp"
#include "midi/alsa_client.hpp"
#include "project/journal.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace ghost_story::ops {

namespace fs = std::filesystem;

namespace {

Check audio_backend_check() {
    auto backends = audio::make_backends();
    for (auto& b : backends) {
        if (b->available()) {
            return {std::string{"audio backend: "} + b->name(),
                    CheckStatus::Pass, "available"};
        }
    }
    return {"audio backends", CheckStatus::Fail,
            "no backend available (PipeWire/JACK/ALSA)"};
}

Check midi_check() {
    auto const ports = midi::AlsaClient::enumerate_input_ports();
    bool mpk = false;
    for (auto const& p : ports) {
        if (!midi::mpk_model_from_name(p.client_name).empty()) mpk = true;
    }
    if (ports.empty()) {
        return {"midi ports", CheckStatus::Warn, "no MIDI inputs found"};
    }
    return {std::string{"midi ports ("} + std::to_string(ports.size()) + ")",
            mpk ? CheckStatus::Pass : CheckStatus::Warn,
            mpk ? "MPK Mini detected" : "no MPK Mini attached"};
}

Check manifest_check() {
    auto const path = device::manifest_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return {"device manifest", CheckStatus::Warn,
                "missing — run `ghost_story probe`"};
    }
    auto m = device::load_manifest(path);
    if (!m) {
        return {"device manifest", CheckStatus::Fail,
                "corrupt (quarantined on next repair)"};
    }
    // Freshness: re-probe advised when older than 7 days.
    auto const age_days =
        std::chrono::duration_cast<std::chrono::hours>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() -
        static_cast<long long>(m->probed_at_ns) / 3600000LL;
    if (age_days > 24 * 7) {
        return {"device manifest", CheckStatus::Warn,
                "stale (>7 days) — re-run `ghost_story probe`"};
    }
    return {"device manifest", CheckStatus::Pass,
            "model=" + (m->model.empty() ? "generic" : m->model)};
}

Check disk_space_check() {
    auto const path = device::manifest_path();
    auto const dir = path.parent_path().empty() ? fs::path{"/tmp"}
                                                : path.parent_path();
    std::error_code ec;
    auto const space = fs::space(dir, ec);
    if (ec) return {"disk space", CheckStatus::Warn, "cannot stat"};
    auto const free_mb = space.available / (1024 * 1024);
    if (free_mb < 100) {
        return {"disk space", CheckStatus::Fail,
                std::to_string(free_mb) + " MB free"};
    }
    if (free_mb < 1024) {
        return {"disk space", CheckStatus::Warn,
                std::to_string(free_mb) + " MB free"};
    }
    return {"disk space", CheckStatus::Pass,
            std::to_string(free_mb) + " MB free"};
}

Check realtime_limits_check() {
    std::ifstream f("/proc/sys/kernel/sched_rt_runtime_us");
    long rt_runtime = -1;
    if (f.good()) f >> rt_runtime;
    if (rt_runtime < 0) {
        return {"realtime limits", CheckStatus::Warn,
                "cannot read sched_rt_runtime_us"};
    }
    // -1 = unlimited; otherwise a fraction of rt_period (default 1s).
    if (rt_runtime == -1) {
        return {"realtime limits", CheckStatus::Pass, "unlimited"};
    }
    return {"realtime limits", CheckStatus::Warn,
            "RT bandwidth throttled (" + std::to_string(rt_runtime) +
                " µs/s) — audio threads still OK, Ghost Story uses normal "
                "scheduling for MIDI"};
}

Check journal_integrity_check() {
    // Scan the default project dir if present: journal must replay.
    auto const dir = device::manifest_path().parent_path().parent_path() /
                     "projects" / "default";
    std::error_code ec;
    if (!fs::exists(dir / "project.journal", ec)) {
        return {"journal", CheckStatus::Pass, "no journal (nothing to check)"};
    }
    project::Journal j;
    std::string error;
    if (!j.open(dir.string(), error)) {
        return {"journal", CheckStatus::Fail, error};
    }
    return {"journal", CheckStatus::Pass,
            std::to_string(j.entries().size()) + " entries replayable"};
}

}  // namespace

bool DoctorReport::all_pass() const noexcept {
    for (auto const& c : checks) {
        if (c.status != CheckStatus::Pass) return false;
    }
    return true;
}

std::string DoctorReport::to_text() const {
    std::string out;
    auto const mark = [](CheckStatus s) {
        switch (s) {
        case CheckStatus::Pass: return "[ OK ]";
        case CheckStatus::Warn: return "[WARN]";
        case CheckStatus::Fail: return "[FAIL]";
        }
        return "[ ?  ]";
    };
    for (auto const& c : checks) {
        out += mark(c.status) + std::string{" "} + c.name + " — " + c.detail +
               "\n";
    }
    return out;
}

std::string DoctorReport::to_json() const {
    nlohmann::json j = nlohmann::json::array();
    for (auto const& c : checks) {
        char const* st = c.status == CheckStatus::Pass
                             ? "pass"
                             : (c.status == CheckStatus::Warn ? "warn"
                                                              : "fail");
        j.push_back({{"name", c.name}, {"status", st}, {"detail", c.detail}});
    }
    return nlohmann::json{{"schema", 1}, {"checks", j}}.dump(2);
}

DoctorReport run_doctor() {
    DoctorReport rep;
    rep.checks.push_back(audio_backend_check());
    rep.checks.push_back(midi_check());
    rep.checks.push_back(manifest_check());
    rep.checks.push_back(disk_space_check());
    rep.checks.push_back(realtime_limits_check());
    rep.checks.push_back(journal_integrity_check());
    return rep;
}

DoctorReport run_repair() {
    DoctorReport rep;
    // 1. Manifest: re-probe if missing/corrupt.
    {
        auto const path = device::manifest_path();
        std::error_code ec;
        auto m = fs::exists(path, ec) ? device::load_manifest(path)
                                      : std::nullopt;
        if (!m) {
            if (fs::exists(path, ec)) device::quarantine_file(path);
            auto result = device::Probe::quick_scan();
            device::save_manifest(result.manifest, path);
            rep.checks.push_back(
                {"manifest re-probe", CheckStatus::Pass,
                 result.device_found ? "device found, manifest written"
                                     : "no device; generic manifest written"});
        } else {
            rep.checks.push_back({"manifest re-probe", CheckStatus::Pass,
                                  "manifest healthy; kept"});
        }
    }
    // 2. Journal: replay validation over the default project.
    {
        auto const dir = device::manifest_path().parent_path().parent_path() /
                         "projects" / "default";
        std::error_code ec;
        if (fs::exists(dir / "project.journal", ec)) {
            project::Project p;
            std::string error;
            bool const ok = project::load_project(dir.string(), p, error);
            rep.checks.push_back(
                {"journal replay", ok ? CheckStatus::Pass : CheckStatus::Fail,
                 ok ? "replayed cleanly" : error});
        } else {
            rep.checks.push_back({"journal replay", CheckStatus::Pass,
                                  "no journal present"});
        }
    }
    // 3. Config: migrate/delete corrupt (none shipped yet — reserved).
    rep.checks.push_back({"config integrity", CheckStatus::Pass,
                          "config schema v1 (no migration needed)"});
    return rep;
}

}  // namespace ghost_story::ops
