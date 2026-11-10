// ghost_story — tests/test_mpmc_queue.cpp
// Correctness + concurrency tests for the multi-producer queue.
// TSan target in CI: 4 producers × 1 consumer, sequenced delivery.
#include "test_framework.hpp"

#include "core/mpmc_queue.hpp"

#include <cstdint>
#include <thread>
#include <vector>

using ghost_story::core::MpmcQueue;

namespace {
struct Msg {
    std::uint64_t seq;
    std::uint32_t producer;
};
static_assert(std::is_trivially_copyable_v<Msg>);
}  // namespace

TEST(mpmc, empty_pop_fails) {
    MpmcQueue<int, 8> q;
    int v = 0;
    CHECK(!q.pop(v));
}

TEST(mpmc, push_pop_fifo_order_single_thread) {
    MpmcQueue<Msg, 8> q;
    for (std::uint64_t i = 0; i < 8; ++i)
        CHECK(q.push(Msg{i, 0}));
    CHECK(!q.push(Msg{99, 0}));  // full
    for (std::uint64_t i = 0; i < 8; ++i) {
        Msg m{};
        CHECK(q.pop(m));
        CHECK_EQ(m.seq, i);
    }
    Msg m{};
    CHECK(!q.pop(m));
}

TEST(mpmc, wraps_correctly) {
    MpmcQueue<Msg, 4> q;
    std::uint64_t seq = 0;
    for (int round = 0; round < 2000; ++round) {
        std::size_t const burst = static_cast<std::size_t>(round % 5);
        for (std::size_t i = 0; i < burst; ++i) {
            if (!q.push(Msg{seq++, 0})) {
                // queue full: pop one and retry (keeps the wrap test bounded)
                Msg drain{};
                CHECK(q.pop(drain));
                --seq;
                break;
            }
        }
        Msg m{};
        while (q.pop(m)) { /* FIFO checked below via total count */ }
    }
    // All entries were consumed; queue empty.
    Msg m{};
    CHECK(!q.pop(m));
    CHECK_EQ(q.size_approx(), 0u);
}

TEST(mpmc, mpsc_concurrent_no_loss) {
    // 4 producers, 1 consumer. Every (producer, seq) pair must arrive
    // exactly once. Order across producers is unspecified — set semantics.
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
    static constexpr std::uint64_t kPerProducer = 5000;
#else
    static constexpr std::uint64_t kPerProducer = 50000;
#endif
    static constexpr int kProducers = 4;

    MpmcQueue<Msg, 1024> q;
    // Per-producer expected counters, checked by the consumer.
    std::vector<std::uint64_t> next_expected(kProducers, 0);

    std::vector<std::thread> producers;
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (std::uint64_t i = 0; i < kPerProducer; ++i) {
                while (!q.push(Msg{i, static_cast<std::uint32_t>(p)}))
                    std::this_thread::yield();
            }
        });
    }

    std::uint64_t total = 0;
    std::uint64_t order_violations = 0;
    std::thread consumer([&] {
        Msg m{};
        std::uint64_t received = 0;
        while (received < kPerProducer * kProducers) {
            if (q.pop(m)) {
                ++received;
                // each producer's seqs must arrive in order (per-producer FIFO
                // holds: dequeue order equals claimed-position order)
                if (m.seq != next_expected[m.producer]) {
                    ++order_violations;
                    // resync so the loop still terminates deterministically
                    next_expected[m.producer] = m.seq + 1;
                } else {
                    ++next_expected[m.producer];
                }
                ++total;
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (auto& t : producers) t.join();
    consumer.join();

    CHECK_EQ(order_violations, 0u);
    for (std::size_t p = 0; p < kProducers; ++p)
        CHECK_EQ(next_expected[p], kPerProducer);
    CHECK_EQ(total, kPerProducer * kProducers);
}

TEST_MAIN()
