// ghost_story — tests/test_watchdog.cpp
#include "test_framework.hpp"

#include "ops/watchdog.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using ghost_story::ops::Watchdog;

TEST(watchdog, healthy_thread_never_trips) {
    auto& w = Watchdog::instance();
    std::atomic<std::uint64_t> hb{0};
    int const id = w.register_thread({"healthy", &hb, nullptr, 500});
    CHECK(id >= 0);
    w.start();
    for (int i = 0; i < 10; ++i) {
        Watchdog::beat(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_EQ(w.incidents_fired(), 0u);
    w.stop();
    w.deregister(id);
}

TEST(watchdog, stalled_thread_trips_and_recovers) {
    auto& w = Watchdog::instance();
    std::atomic<std::uint64_t> hb{0};
    std::atomic<int> recoveries{0};
    int const id = w.register_thread(
        {"stalled", &hb,
         [&] { recoveries.fetch_add(1); }, 400});
    w.start();
    hb.store(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count()));
    // Never beat again — monitor must fire within ~2.5 s.
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline &&
           recoveries.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(recoveries.load() >= 1);
    CHECK(w.incidents_fired() >= 1);
    w.stop();
    w.deregister(id);
}

TEST(watchdog, deregistered_thread_ignored) {
    auto& w = Watchdog::instance();
    auto const baseline = w.incidents_fired();
    std::atomic<std::uint64_t> hb{0};
    int const id = w.register_thread({"gone", &hb, nullptr, 200});
    w.deregister(id);
    w.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK_EQ(w.incidents_fired(), baseline);  // no NEW incidents
    w.stop();
}

TEST_MAIN()
