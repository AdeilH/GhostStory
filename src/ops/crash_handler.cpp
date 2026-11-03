// addu94 — src/ops/crash_handler.cpp
#include "ops/crash_handler.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <execinfo.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace addu94::ops {

namespace {

// Referenced by sigaction at install(); GCC's unused-function analysis
// can miss the table-driven reference.
[[maybe_unused]]
int g_report_fd{-1};

void write_all(int fd, char const* s) noexcept {
    if (fd < 0 || s == nullptr) return;
    auto const len = std::strlen(s);
    std::size_t off = 0;
    while (off < len) {
        auto const n = ::write(fd, s + off, len - off);
        if (n <= 0) return;
        off += static_cast<std::size_t>(n);
    }
}

[[maybe_unused]] void signal_handler(int sig) noexcept {
    // Async-signal-safe: write(2), backtrace/backtrace_symbols_fd, time.
    if (g_report_fd >= 0) {
        write_all(g_report_fd, "{\"signal\":");
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", sig);
        write_all(g_report_fd, buf);
        write_all(g_report_fd, ",\"name\":\"");
        // Signal-name lookup table (static, handler-safe).
        static char const* const names[] = {"", "SIGHUP", "SIGINT", "SIGQUIT",
                                            "SIGILL", "SIGTRAP", "SIGABRT",
                                            "SIGBUS", "SIGFPE", "SIGKILL",
                                            "SIGUSR1", "SIGSEGV"};
        write_all(g_report_fd,
                  sig > 0 && sig < 12 ? names[sig] : "unknown");
        write_all(g_report_fd, "\",\"backtrace\":[");
        void* frames[64];
        int const n = ::backtrace(frames, 64);
        for (int i = 0; i < n; ++i) {
            if (i > 0) write_all(g_report_fd, ",");
            std::snprintf(buf, sizeof(buf), "\"frame %d\"", i);
            write_all(g_report_fd, buf);
        }
        write_all(g_report_fd, "]}\n");
        // Human-readable backtrace after the JSON line.
        ::backtrace_symbols_fd(frames, n, g_report_fd);
    }
    // Re-raise with default handling so core dumps still work.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}  // namespace

bool CrashHandler::install() {
    auto const dir = crash_dir();
    char path[512];
    std::snprintf(path, sizeof(path), "%s/crash-%lld.jsonl", dir.c_str(),
                  static_cast<long long>(::time(nullptr)));
    ::mkdir(dir.c_str(), 0755);
    g_report_fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);


    struct SigEntry { int sig; char const* name; };
    static SigEntry const entries[] = {
        {SIGSEGV, "SIGSEGV"}, {SIGABRT, "SIGABRT"}, {SIGFPE, "SIGFPE"},
        {SIGBUS, "SIGBUS"}, {SIGILL, "SIGILL"},
    };
    for (auto const& e : entries) {
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        ::sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(e.sig, &sa, nullptr);
    }

    // Safe-mode detection: flag file exists = last run crashed or failed.
    char flag[512];
    std::snprintf(flag, sizeof(flag), "%s/safe-mode-flag", dir.c_str());
    bool const crashed = ::access(flag, F_OK) == 0;
    // Re-arm: this run has not proven healthy yet.
    int fd = ::open(flag, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) ::close(fd);
    return crashed;
}

void CrashHandler::clear_safe_mode_flag() {
    char flag[512];
    std::snprintf(flag, sizeof(flag), "%s/safe-mode-flag",
                  crash_dir().c_str());
    ::unlink(flag);
}

std::string CrashHandler::crash_dir() {
    char const* home = ::getenv("XDG_DATA_HOME");
    std::string base = (home != nullptr && home[0] != '\0')
                           ? home
                           : std::string{::getenv("HOME") != nullptr
                                             ? ::getenv("HOME")
                                             : "."} +
                                 "/.local/share";
    return base + "/addu94/crashes";
}

}  // namespace addu94::ops
