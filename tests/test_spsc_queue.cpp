// ghost_story — tests/test_spsc_queue.cpp
// Correctness + concurrency tests for the foundational RT primitive.
// The multithreaded test is also run under TSan in CI (see scripts/ci.sh).
#include "test_framework.hpp"

#include "core/spsc_queue.hpp"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using ghost_story::core::SpscQueue;

namespace {

struct Msg {
    std::uint64_t seq;
    std::uint32_t value;
};
static_assert(std::is_trivially_copyable_v<Msg>);

}  // namespace

TEST(spsc, empty_pop_fails) {
    SpscQueue<int, 8> q;
    int v = 0;
    CHECK(!q.pop(v));
    CHECK_EQ(q.size_approx(), 0u);
}

TEST(spsc, push_pop_fifo_order) {
    SpscQueue<Msg, 8> q;
    for (std::uint64_t i = 0; i < 8; ++i) {
        CHECK(q.push(Msg{i, static_cast<std::uint32_t>(i * 3)}));
    }
    CHECK(!q.push(Msg{99, 99}));  // full
    for (std::uint64_t i = 0; i < 8; ++i) {
        Msg m{};
        CHECK(q.pop(m));
        CHECK_EQ(m.seq, i);
        CHECK_EQ(m.value, i * 3);
    }
    Msg m{};
    CHECK(!q.pop(m));
}

TEST(spsc, wraps_without_leak) {
    // Non-power-of-two usage pattern over many wraps.
    SpscQueue<Msg, 4> q;
    std::uint64_t produced = 0, consumed = 0;
    for (int round = 0; round < 1000; ++round) {
        std::size_t const burst = static_cast<std::size_t>(round % 6);
        for (std::size_t i = 0; i < burst; ++i) {
            if (q.push(Msg{produced, 1u})) ++produced;
        }
        Msg m{};
        while (q.pop(m)) {
            CHECK_EQ(m.seq, consumed);
            ++consumed;
        }
    }
    CHECK_EQ(produced, consumed);
}

TEST(spsc, nontrivial_type_move_only) {
    struct MoveOnly {
        explicit MoveOnly(int val) : v(val) {}
        MoveOnly(MoveOnly const&) = delete;
        MoveOnly& operator=(MoveOnly const&) = delete;
        MoveOnly(MoveOnly&&) noexcept = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;
        int v;
    };
    SpscQueue<MoveOnly, 4> q;
    CHECK(q.emplace(42));
    MoveOnly out{0};
    CHECK(q.pop(out));
    CHECK_EQ(out.v, 42);
}

TEST(spsc, spsc_concurrent_sequenced_delivery) {
    // Producer/consumer on separate threads; every produced seq must be
    // delivered exactly once, in order. This is the TSan target.
    // TSan serialises every atomic access (~15x): shrink the iteration count
    // there so the gate stays minutes, not hours. Coverage of the ordering
    // properties does not depend on volume.
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
    static constexpr std::uint64_t kCount = 20000;
#else
    static constexpr std::uint64_t kCount = 200000;
#endif
    SpscQueue<Msg, 1024> q;
    std::vector<std::uint64_t> gaps;  // empty: success means no gaps

    std::thread producer([&q] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!q.push(Msg{i, 0u})) {
                // queue full — spin; SPSC has no blocking primitive by design
                std::this_thread::yield();
            }
        }
    });
    std::uint64_t expected = 0;
    std::thread consumer([&q, &expected] {
        Msg m{};
        while (expected < kCount) {
            if (q.pop(m)) {
                if (m.seq != expected) break;  // order violation
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK_EQ(expected, kCount);
}

TEST_MAIN()
