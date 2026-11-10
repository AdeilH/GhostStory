// ghost_story — src/ops/telemetry.cpp
#include "ops/telemetry.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

namespace ghost_story::ops {

namespace {
bool g_enabled{false};
std::string telemetry_path() {
    char const* xdg = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base =
        (xdg != nullptr && xdg[0] != '\0')
            ? std::filesystem::path{xdg}
            : ((std::getenv("HOME") != nullptr)
                   ? std::filesystem::path{std::getenv("HOME")} / ".local" /
                         "share"
                   : std::filesystem::path{"."});
    return (base / "ghost_story" / "telemetry" / "sessions.jsonl").string();
}

// ISO-8601 UTC at millisecond precision — matches the log schema so the
// observability stack can time-filter telemetry the same way as logs.
std::string iso8601_utc_now() {
    return std::format(
        "{:%FT%TZ}",
        std::chrono::floor<std::chrono::milliseconds>(
            std::chrono::system_clock::now()));
}
}  // namespace

void Telemetry::init(bool enabled) { g_enabled = enabled; }
void Telemetry::shutdown() noexcept { g_enabled = false; }

void Telemetry::event(std::string const& kind, std::string const& detail) {
    if (!g_enabled) return;
    nlohmann::json j = {
        {"ts_ns", static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count())},
        {"@timestamp", iso8601_utc_now()},
        {"kind", kind},
        {"detail", detail},
    };
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path{telemetry_path()}.parent_path(), ec);
    std::ofstream f(telemetry_path(), std::ios::app);
    if (f.good()) f << j.dump() << "\n";
}

void Telemetry::session_start() {
    event("session_start", "");
}

void Telemetry::session_end(SessionStats const& s) {
    if (!g_enabled) return;
    nlohmann::json j = {
        {"ts_ns", static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count())},
        {"@timestamp", iso8601_utc_now()},
        {"kind", "session_end"},
        {"xruns", s.xruns},
        {"max_callback_us", s.max_callback_us},
        {"midi_events", s.midi_events},
        {"crashes_recovered", s.crashes_recovered},
        {"duration_s", s.duration_s},
    };
    std::ofstream f(telemetry_path(), std::ios::app);
    if (f.good()) f << j.dump() << "\n";
}

std::string Telemetry::load_recent(int max_lines) {
    std::ifstream f(telemetry_path());
    if (!f.good()) return "";
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (static_cast<int>(lines.size()) > max_lines) lines.pop_front();
    }
    std::string out;
    for (auto const& l : lines) out += l + "\n";
    return out;
}

}  // namespace ghost_story::ops
