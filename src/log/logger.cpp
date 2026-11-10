// ghost_story — src/log/logger.cpp
#include "log/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <ostream>
#include <random>
#include <string>
#include <system_error>

namespace ghost_story::log {

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

// ISO-8601 UTC, millisecond precision — the field Elasticsearch/Kibana index
// directly as the document timestamp (schema v2 of the JSONL file).
std::string iso8601_utc(std::uint64_t ns_epoch) noexcept {
    std::chrono::sys_time<std::chrono::nanoseconds> const tp{
        std::chrono::nanoseconds{ns_epoch}};
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::milliseconds>(tp));
}
}  // namespace

std::string default_log_path() {
    char const* xdg = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base =
        (xdg != nullptr && xdg[0] != '\0')
            ? std::filesystem::path{xdg}
            : ((std::getenv("HOME") != nullptr)
                   ? std::filesystem::path{std::getenv("HOME")} / ".local" /
                         "share"
                   : std::filesystem::path{"."});
    return (base / "ghost_story" / "logs" / "app.jsonl").string();
}

Logger& Logger::instance() noexcept {
    static Logger g;
    return g;
}

// Per-thread label for the "th" field. POD TLS: no init/destroy, a plain
// fs-relative read on access — safe on the RT path.
thread_local char t_thread_name[Entry::kThreadNameMax] = {};

void Logger::set_thread_name(std::string_view name) noexcept {
    (void)bounded_copy(t_thread_name, Entry::kThreadNameMax, name);
}

std::string_view Logger::this_thread_name() noexcept {
    return {t_thread_name, std::strlen(t_thread_name)};
}

std::uint64_t Logger::now_ns() noexcept {
    // Wall-clock (not steady): timestamps must line up with telemetry,
    // incidents and the @timestamp field shipped to Elasticsearch.
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
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
    l.run_id_ = std::format("{:08x}", std::random_device{}());
    set_thread_name("main");
    if (!jsonl_path.empty()) {
        std::error_code ec;
        std::filesystem::path const path{jsonl_path};
        std::filesystem::create_directories(path.parent_path(), ec);
        l.file_path_ = std::string{jsonl_path};
        // Bound the retained history: an oversized file from a previous run
        // becomes ".1" (replacing an older one), never a bottomless pile.
        if (std::filesystem::file_size(path, ec) >= kRotateBytes && !ec) {
            std::filesystem::rename(
                path, std::filesystem::path{l.file_path_ + ".1"}, ec);
            (void)ec;
        }
        l.open_file(jsonl_path);
    }
    l.running_.store(true, std::memory_order_release);
    l.writer_ = std::thread([&l] { l.run(); });
}

void Logger::open_file(std::string_view path) noexcept {
    file_ = std::fopen(std::string{path}.c_str(), "a");
    if (file_ != nullptr) {
        // Line-buffered so crashes lose at most the current line.
        std::setvbuf(file_, nullptr, _IOLBF, 4096);
        std::error_code ec;
        file_bytes_ = std::filesystem::file_size(
            std::filesystem::path{std::string{path}}, ec);
        if (ec) file_bytes_ = 0;
    } else {
        std::cerr << "[ghost_story] failed to open log file: " << path << "\n";
    }
}

void Logger::rotate_file() noexcept {
    if (file_ == nullptr) return;
    std::FILE* f = file_;
    file_ = nullptr;
    std::fclose(f);
    file_bytes_ = 0;
    std::error_code ec;
    std::filesystem::path const current{file_path_};
    std::filesystem::rename(current,
                            std::filesystem::path{current.string() + ".1"}, ec);
    (void)ec;
    open_file(file_path_);
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

std::string_view Logger::run_id() noexcept {
    return instance().run_id_;
}

void Logger::run() {
    set_thread_name("log");
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
    std::string_view const th{e.thread_name, e.thread_name_len};

    // Human-readable line to stderr for Warning and above.
    if (e.level >= stderr_level_) {
        std::string const line = std::format(
            "[{:>5}] {}{}: {}\n", to_string(e.level), sub,
            th.empty() ? "" : std::format("({})", th), msg);
        std::fwrite(line.data(), 1, line.size(), stderr);
    }

    if (file_ == nullptr) return;

    char esc[256];
    std::string_view esc_msg = json_escape(msg, esc);
    // Schema v3: run id (group one process run in Kibana) + thread label.
    std::string const j = std::format(
        "{{\"ts_ns\":{},\"@timestamp\":\"{}\",\"run\":\"{}\",\"lvl\":\"{}\","
        "\"tid\":{},\"th\":\"{}\",\"sub\":\"{}\",\"msg\":\"{}\"}}\n",
        e.timestamp_ns, iso8601_utc(e.timestamp_ns), run_id_,
        to_string(e.level), e.tid, th, sub, esc_msg);
    std::fwrite(j.data(), 1, j.size(), file_);
    file_bytes_ += j.size();
    if (file_bytes_ >= kRotateBytes) rotate_file();
}

}  // namespace ghost_story::log
