// addu94 — src/log/logger.cpp
#include "log/logger.hpp"

#include <format>
#include <iostream>
#include <ostream>

namespace addu94::log {

namespace {
std::string_view json_escape(std::string_view in, char (&out)[256]) noexcept {
    std::size_t n = 0;
    for (char const c : in) {
        if (n >= 250) break;
        switch (c) {
        case '"': out[n++] = '\\'; out[n++] = '"'; break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        case '\n': out[n++] = '\\'; out[n++] = 'n'; break;
        case '\r': out[n++] = '\\'; out[n++] = 'r'; break;
        case '\t': out[n++] = '\\'; out[n++] = 't'; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out[n++] = ' ';
            } else {
                out[n++] = c;
            }
        }
    }
    out[n] = '\0';
    return std::string_view{out, n};
}
}  // namespace

Logger& Logger::instance() noexcept {
    static Logger g;
    return g;
}

std::uint64_t Logger::now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint64_t Logger::this_thread_id() noexcept {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(
        std::this_thread::get_id()));
}

std::size_t Logger::bounded_copy(char* dst, std::size_t cap,
                                 std::string_view src) noexcept {
    std::size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    __builtin_memcpy(dst, src.data(), n);
    dst[n] = '\0';
    return n;
}

void Logger::init(std::string_view jsonl_path, Level stderr_level) {
    Logger& l = instance();
    l.stderr_level_ = stderr_level;
    if (!jsonl_path.empty()) {
        l.file_ = std::fopen(std::string{jsonl_path}.c_str(), "a");
        if (l.file_ != nullptr) {
            // Line-buffered so crashes lose at most the current line.
            std::setvbuf(l.file_, nullptr, _IOLBF, 4096);
        } else {
            std::cerr << "[addu94] failed to open log file: " << jsonl_path
                      << "\n";
        }
    }
    l.running_.store(true, std::memory_order_release);
    l.writer_ = std::thread([&l] { l.run(); });
}

void Logger::shutdown() noexcept {
    Logger& l = instance();
    if (!l.running_.exchange(false, std::memory_order_acq_rel)) return;
    if (l.writer_.joinable()) l.writer_.join();
    if (l.file_ != nullptr) {
        std::fclose(l.file_);
        l.file_ = nullptr;
    }
}

std::uint64_t Logger::dropped_entries() noexcept {
    return instance().dropped_.load(std::memory_order_relaxed);
}

void Logger::run() {
    Entry e{};
    while (running_.load(std::memory_order_acquire)) {
        if (queue_.pop(e)) {
            write_entry(e);
            continue;
        }
        // Brief pause when idle. Cannot be a plain nanosleep: TSan models
        // sleeps as clock-sync points and loses the SPSC/MPMC happens-before
        // edge across it (observed false-positive races on cell data).
        for (int spin = 0; spin < 1000 && !running_.load(std::memory_order_relaxed); ++spin) {
            #if defined(__x86_64__)
            __builtin_ia32_pause();
            #endif
        }
    }
    // Drain remaining entries after shutdown flag.
    while (queue_.pop(e)) write_entry(e);
}

void Logger::write_entry(Entry const& e) noexcept {
    std::string_view sub{e.subsystem, e.subsystem_len};
    std::string_view msg{e.message, e.message_len};

    // Human-readable line to stderr for Warning and above.
    if (e.level >= stderr_level_) {
        std::string const line =
            std::format("[{:>5}] {}: {}\n", to_string(e.level), sub, msg);
        std::fwrite(line.data(), 1, line.size(), stderr);
    }

    if (file_ == nullptr) return;

    char esc[256];
    std::string_view esc_msg = json_escape(msg, esc);
    std::string const j = std::format(
        "{{\"ts\":{},\"lvl\":\"{}\",\"tid\":{},\"sub\":\"{}\",\"msg\":\"{}\"}}\n",
        e.timestamp_ns, to_string(e.level), e.tid, sub, esc_msg);
    std::fwrite(j.data(), 1, j.size(), file_);
}

}  // namespace addu94::log
