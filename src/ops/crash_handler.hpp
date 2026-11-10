// ghost_story — src/ops/crash_handler.hpp
//
// Crash handler (spec §6.4): captures signal (SIGSEGV/SIGABRT/SIGFPE/
// SIGBUS/SIGILL, plus the termination signals SIGTERM/SIGINT/SIGHUP so a
// killed process is never indistinguishable from a segfaulted one — the
// zero-byte-report ambiguity), writes a crash report (backtrace) into
// ~/.local/share/ghost_story/crashes/, and marks the safe-mode flag so the
// next launch offers diagnostics-first startup.
//
// Report-file lifecycle: install() pre-opens crash-<start-epoch>.jsonl
// (async-signal-safe writes go there). A CLEAN shutdown removes it via
// mark_clean_shutdown() — a zero-byte report that survives means the
// process died without any caught signal (SIGKILL most likely); a
// non-empty one always names its signal.
//
// Signal-handler constraints: only async-signal-safe calls. We pre-open
// the report fd and pre-format nothing; backtrace_symbols_fd is used.
#pragma once

#include <cstdint>
#include <string>

namespace ghost_story::ops {

class CrashHandler {
public:
    /// Install handlers + safe-mode detection. Call once from main.
    /// Returns true if the LAST run crashed (safe mode should be offered).
    static bool install();

    /// Clear the safe-mode flag (call after a successful healthy start).
    static void clear_safe_mode_flag();

    /// Clean shutdown bookkeeping: removes this run's still-empty report
    /// file and clears the safe-mode flag. Call once from the normal exit
    /// path (after the engine stopped). A run killed mid-session never
    /// reaches this, which is exactly what makes the flag meaningful.
    static void mark_clean_shutdown();

    [[nodiscard]] static std::string crash_dir();
};

}  // namespace ghost_story::ops
