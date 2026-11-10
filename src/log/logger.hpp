// ghost_story — src/log/logger.hpp
//
// Asynchronous structured logger.
//
// Design: log() formats into a fixed-size entry (no allocation, no locks)
// and pushes it onto a lock-free SPSC queue. A background thread drains the
// queue and writes JSONL (machine) and human-readable lines. This makes
// log() callable from the audio and MIDI threads (spec §3 hard rule: no
// logging with locks/allocations on the RT path).
//
// Under queue pressure entries are DROPPED, never blocking the producer —
// a dropped log is counted and surfaced in stats (never silent data loss of
// *program* data; logs are diagnostics, and blocking RT threads to preserve
// them would be worse).
#pragma once

#include "core/mpmc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <thread>

namespace ghost_story::log {

enum class Level : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Critical = 5,
};

[[nodiscard]] constexpr std::string_view to_string(Level l) noexcept {
    switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Critical: return "CRIT";
    }
    return "?";
}

/// Fixed-size log entry — trivially copyable so it rides the SPSC queue.
struct Entry {
    std::uint64_t timestamp_ns;
    Level level;
    std::uint64_t tid;
    static constexpr std::size_t kSubMax = 20;
    static constexpr std::size_t kMsgMax = 220;
    static constexpr std::size_t kThreadNameMax = 12;
    std::size_t subsystem_len;
    std::size_t message_len;
    std::size_t thread_name_len;
    char subsystem[kSubMax];
    char message[kMsgMax];
    char thread_name[kThreadNameMax];
};

/// Default JSONL log file for interactive runs:
/// `$XDG_DATA_HOME|~/.local/share` + `/ghost_story/logs/app.jsonl`.
/// Same XDG base pattern as ops/telemetry.cpp.
[[nodiscard]] std::string default_log_path();

class Logger {
public:
    /// Existing log file at or above this size is renamed to "<name>.1"
    /// (replacing any previous ".1") on init and mid-session by the writer
    /// thread, so the JSONL never grows without bound.
    static constexpr std::uint64_t kRotateBytes = 32u * 1024u * 1024u;

    /// Initialise the global logger. `jsonl_path` may be empty (stderr only).
    /// The parent directory of `jsonl_path` is created if missing.
    /// Generates this run's id (8 hex chars) — every log line carries it, so
    /// Kibana can group/correlate one process run across log, telemetry and
    /// incidents. Must be called once from main before any log() call.
    static void init(std::string_view jsonl_path, Level stderr_level);
    static void shutdown() noexcept;

    /// Label THIS thread in every entry it logs afterwards ("audio",
    /// "midi", "watchdog", …). Call once at the top of a thread's loop or
    /// callback; init() names the calling thread "main". Bounded copy —
    /// safe on the RT path.
    static void set_thread_name(std::string_view name) noexcept;

    /// Random 8-hex id of the current run (empty before init).
    [[nodiscard]] static std::string_view run_id() noexcept;

    /// Realtime-safe, wait-free, allocation-free (bounded formatting).
    template <typename... Args>
    static void log(Level level, std::string_view subsystem,
                    std::format_string<Args...> fmt, Args&&... args) {
        Logger& l = instance();
        Entry e{};
        e.timestamp_ns = now_ns();
        e.level = level;
        e.tid = this_thread_id();
        e.thread_name_len = bounded_copy(e.thread_name,
                                         Entry::kThreadNameMax,
                                         this_thread_name());
        e.subsystem_len =
            bounded_copy(e.subsystem, Entry::kSubMax, subsystem);
        auto res = std::format_to_n(e.message, Entry::kMsgMax - 1, fmt,
                                    std::forward<Args>(args)...);
        e.message_len = static_cast<std::size_t>(res.size);
        if (e.message_len >= Entry::kMsgMax) e.message_len = Entry::kMsgMax - 1;
        if (!l.queue_.push(e)) l.dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Stats for ops/telemetry.
    [[nodiscard]] static std::uint64_t dropped_entries() noexcept;

    Logger(Logger const&) = delete;
    Logger& operator=(Logger const&) = delete;

private:
    Logger() = default;
    static Logger& instance() noexcept;
    static std::uint64_t now_ns() noexcept;
    static std::uint64_t this_thread_id() noexcept;
    // Reads this thread's label from thread-local storage (set via
    // set_thread_name). Cheap TLS read — RT-safe.
    static std::string_view this_thread_name() noexcept;
    static std::size_t bounded_copy(char* dst, std::size_t cap,
                                    std::string_view src) noexcept;

    void run();
    void write_entry(Entry const& e) noexcept;
    // Writer-thread helpers (file_ is owned by the writer thread only).
    void open_file(std::string_view path) noexcept;
    void rotate_file() noexcept;

    static constexpr std::size_t kQueueCap = 4096;
    core::MpmcQueue<Entry, kQueueCap> queue_;
    std::thread writer_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::FILE* file_{nullptr};  // owned by writer thread only
    std::uint64_t file_bytes_{0};
    std::string file_path_;  // set in init(); read only by the writer thread
    std::string run_id_;     // set in init(); read by producer + writer
    Level stderr_level_{Level::Warning};
};

/// Convenience wrappers.
template <typename... Args>
void trace(std::string_view s, std::format_string<Args...> f, Args&&... a) {
    Logger::log(Level::Trace, s, f, std::forward<Args>(a)...);
}
template <typename... Args>
void debug(std::string_view s, std::format_string<Args...> f, Args&&... a) {
    Logger::log(Level::Debug, s, f, std::forward<Args>(a)...);
}
template <typename... Args>
void info(std::string_view s, std::format_string<Args...> f, Args&&... a) {
    Logger::log(Level::Info, s, f, std::forward<Args>(a)...);
}
template <typename... Args>
void warn(std::string_view s, std::format_string<Args...> f, Args&&... a) {
    Logger::log(Level::Warning, s, f, std::forward<Args>(a)...);
}
template <typename... Args>
void error(std::string_view s, std::format_string<Args...> f, Args&&... a) {
    Logger::log(Level::Error, s, f, std::forward<Args>(a)...);
}
template <typename... Args>
void critical(std::string_view s, std::format_string<Args...> f, Args&&... a) {
    Logger::log(Level::Critical, s, f, std::forward<Args>(a)...);
}

}  // namespace ghost_story::log
