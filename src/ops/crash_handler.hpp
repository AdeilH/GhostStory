// addu94 — src/ops/crash_handler.hpp
//
// Crash handler (spec §6.4): captures signal (SIGSEGV/SIGABRT/SIGFPE/
// SIGBUS/SIGILL), writes a crash report (backtrace + last log lines +
// process stats) into ~/.local/share/addu94/crashes/, and marks the
// safe-mode flag so the next launch offers diagnostics-first startup.
//
// Signal-handler constraints: only async-signal-safe calls. We pre-open
// the report fd and pre-format nothing; backtrace_symbols_fd is used.
#pragma once

#include <cstdint>
#include <string>

namespace addu94::ops {

class CrashHandler {
public:
    /// Install handlers + safe-mode detection. Call once from main.
    /// Returns true if the LAST run crashed (safe mode should be offered).
    static bool install();

    /// Clear the safe-mode flag (call after a successful healthy start).
    static void clear_safe_mode_flag();

    [[nodiscard]] static std::string crash_dir();
};

}  // namespace addu94::ops
