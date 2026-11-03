// addu94 — src/ops/watchdog.hpp
//
// Watchdog (spec §6.1): registered threads push heartbeats (atomic ns
// timestamps); the monitor thread checks every second. Two consecutive
// missed beats → structured incident (JSONL) + registered recovery hook.
// The monitor never blocks or locks watched threads: heartbeats are plain
// atomic stores by the watched thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <array>
#include <functional>
#include <thread>
#include <string>
#include <vector>

namespace addu94::ops {

class Watchdog {
public:
    struct Registration {
        std::string name;
        std::atomic<std::uint64_t>* heartbeat_ns;  // owned by the watched thread
        std::function<void()> recover;             // in-process restart hook
        std::uint64_t timeout_ms{2000};
    };

    static Watchdog& instance() noexcept;

    /// Control thread: register a subsystem. Pointer must outlive the
    /// registration (or deregister first). Returns an id (-1 on error).
    int register_thread(Registration reg);
    void deregister(int id) noexcept;

    /// Called by watched threads (RT-safe: one relaxed atomic store).
    static void beat(int id) noexcept;

    /// Start/stop the monitor thread.
    bool start();
    void stop() noexcept;

    [[nodiscard]] std::uint64_t incidents_fired() const noexcept {
        return incidents_.load(std::memory_order_relaxed);
    }

private:
    Watchdog() = default;
    void monitor_loop();

    struct Slot {
        Registration reg;
        std::atomic<bool> used{false};
    };
    static constexpr int kMaxSlots = 16;
    std::array<Slot, kMaxSlots> slots_{};
    std::atomic<int> next_id_{0};
    std::atomic<std::uint64_t> incidents_{0};
    std::atomic<bool> running_{false};
    std::thread monitor_;
};

/// Append a structured incident to ~/.local/share/addu94/incidents.jsonl
/// (never throws; best-effort I/O from any thread).
void log_incident(std::string const& subsystem, std::string const& kind,
                  std::string const& detail);

}  // namespace addu94::ops
