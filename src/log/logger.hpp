// addu94 — src/log/logger.hpp
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

namespace addu94::log {

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
    std::size_t subsystem_len;
    std::size_t message_len;
    char subsystem[kSubMax];
    char message[kMsgMax];
};

class Logger {
public:
    /// Initialise the global logger. `jsonl_path` may be empty (stderr only).
    /// Must be called once from main before any log() call.
    static void init(std::string_view jsonl_path, Level stderr_level);
    static void shutdown() noexcept;

    /// Realtime-safe, wait-free, allocation-free (bounded formatting).
    template <typename... Args>
    static void log(Level level, std::string_view subsystem,
                    std::format_string<Args...> fmt, Args&&... args) {
        Logger& l = instance();
        Entry e{};
        e.timestamp_ns = now_ns();
        e.level = level;
        e.tid = this_thread_id();
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
    static std::size_t bounded_copy(char* dst, std::size_t cap,
                                    std::string_view src) noexcept;

    void run();
    void write_entry(Entry const& e) noexcept;

    static constexpr std::size_t kQueueCap = 4096;
    core::MpmcQueue<Entry, kQueueCap> queue_;
    std::thread writer_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::FILE* file_{nullptr};  // owned by writer thread only
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

}  // namespace addu94::log
