// ghost_story — src/ops/watchdog.cpp
#include "ops/watchdog.hpp"

#include "device/manifest.hpp"
#include "log/logger.hpp"

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

namespace ghost_story::ops {

namespace {
std::uint64_t now_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

Watchdog& Watchdog::instance() noexcept {
    static Watchdog w;
    return w;
}

int Watchdog::register_thread(Registration reg) {
    int const id = next_id_.fetch_add(1, std::memory_order_relaxed);
    if (id < 0 || id >= kMaxSlots) return -1;
    auto& slot = slots_[static_cast<std::size_t>(id)];
    // A fresh registration starts "healthy": the first beat may be seconds
    // away and a zeroed heartbeat must not count as a stall.
    if (reg.heartbeat_ns != nullptr) {
        reg.heartbeat_ns->store(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()),
            std::memory_order_release);
    }
    slot.reg = std::move(reg);
    slot.used.store(true, std::memory_order_release);
    log::info("watchdog", "watching {} (timeout {} ms)", slot.reg.name,
              slot.reg.timeout_ms);
    return id;
}

void Watchdog::deregister(int id) noexcept {
    if (id < 0 || id >= kMaxSlots) return;
    slots_[static_cast<std::size_t>(id)].used.store(false,
                                                    std::memory_order_release);
}

void Watchdog::beat(int id) noexcept {
    if (id < 0 || id >= kMaxSlots) return;
    auto& slot = Watchdog::instance().slots_[static_cast<std::size_t>(id)];
    if (slot.reg.heartbeat_ns != nullptr) {
        slot.reg.heartbeat_ns->store(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()),
            std::memory_order_release);
    }
}

bool Watchdog::start() {
    if (running_.exchange(true)) return true;
    try {
        monitor_ = std::thread([this] { monitor_loop(); });
    } catch (...) {
        running_.store(false);
        return false;
    }
    return true;
}

void Watchdog::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (monitor_.joinable()) monitor_.join();
}

void Watchdog::monitor_loop() {
    ghost_story::log::Logger::set_thread_name("watchdog");
    while (running_.load(std::memory_order_acquire)) {
        auto const now = now_ms();
        for (auto& slot : slots_) {
            if (!slot.used.load(std::memory_order_acquire)) continue;
            auto const hb = slot.reg.heartbeat_ns != nullptr
                                ? slot.reg.heartbeat_ns->load(
                                      std::memory_order_acquire)
                                : now;
            // heartbeat is in ns; convert both to ms.
            auto const hb_ms = hb / 1'000'000ULL;
            if (now - hb_ms > slot.reg.timeout_ms) {
                incidents_.fetch_add(1, std::memory_order_relaxed);
                log_incident("watchdog", "stall",
                             slot.reg.name + " missed heartbeat for " +
                                 std::to_string(now - hb_ms) + " ms");
                if (slot.reg.recover) {
                    // Recovery runs on the monitor thread — hooks must not
                    // touch the audio callback directly (they queue state).
                    slot.reg.recover();
                }
                // Consider it beaten so we don't fire every second while
                // the recovery takes hold; the subsystem re-beats anyway.
                if (slot.reg.heartbeat_ns != nullptr) {
                    slot.reg.heartbeat_ns->store(
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch())
                                .count()),
                        std::memory_order_release);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void log_incident(std::string const& subsystem, std::string const& kind,
                  std::string const& detail) {
    log::error(subsystem.c_str() ? "watchdog" : "watchdog", "{}: {}", kind,
               detail);
    nlohmann::json j = {
        {"ts_ns", static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now()
                              .time_since_epoch())
                          .count())},
        {"subsystem", subsystem},
        {"kind", kind},
        {"detail", detail},
    };
    auto path = device::manifest_path().parent_path().parent_path() /
                "incidents.jsonl";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::app);
    if (f.good()) f << j.dump() << "\n";
}

}  // namespace ghost_story::ops
