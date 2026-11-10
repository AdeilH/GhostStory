// ghost_story — src/core/mpmc_queue.hpp
//
// Bounded MPMC ring buffer (Dmitry Vyukov's algorithm), used where multiple
// threads log or emit events concurrently (logger, ops events). The audio
// hot path keeps SpscQueue (single producer, cheaper).
//
// Push is lock-free (one CAS on the enqueue position); pop is lock-free.
// No allocation, no locks, no syscalls — safe on the RT path, TSan-tested.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "core/spsc_queue.hpp"  // kCacheLine

namespace ghost_story::core {

template <typename T, std::size_t CapacityPow2>
class MpmcQueue {
    static_assert(CapacityPow2 != 0 &&
                      (CapacityPow2 & (CapacityPow2 - 1)) == 0,
                  "CapacityPow2 must be a power of two");
    static_assert(std::is_nothrow_move_constructible_v<T> ||
                      std::is_trivially_copyable_v<T>,
                  "T must be noexcept-movable or trivially copyable");

    struct Cell {
        std::atomic<std::size_t> seq;
        T value;
    };

public:
    MpmcQueue() noexcept {
        for (std::size_t i = 0; i < CapacityPow2; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
    }
    MpmcQueue(MpmcQueue const&) = delete;
    MpmcQueue& operator=(MpmcQueue const&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return CapacityPow2;
    }

    /// Lock-free. Returns false when full (element NOT enqueued).
    template <typename... Args>
    [[nodiscard]] bool emplace(Args&&... args) noexcept {
        Cell* cell = nullptr;
        std::size_t pos =
            enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            std::size_t const seq =
                cell->seq.load(std::memory_order_acquire);
            auto const dif = static_cast<std::ptrdiff_t>(seq) -
                             static_cast<std::ptrdiff_t>(pos);
            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
                // CAS failed: pos reloaded, retry with new pos.
            } else if (dif < 0) {
                return false;  // queue full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        ::new (static_cast<void*>(&cell->value))
            T(std::forward<Args>(args)...);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool push(T value) noexcept {
        return emplace(std::move(value));
    }

    /// Lock-free. Returns false when empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            std::size_t const seq =
                cell->seq.load(std::memory_order_acquire);
            auto const dif = static_cast<std::ptrdiff_t>(seq) -
                             static_cast<std::ptrdiff_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false;  // queue empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->value);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            cell->value.~T();
        }
        cell->seq.store(pos + CapacityPow2, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        auto const enq = enqueue_pos_.load(std::memory_order_relaxed);
        auto const deq = dequeue_pos_.load(std::memory_order_relaxed);
        return enq - deq;
    }

private:
    static constexpr std::size_t kMask = CapacityPow2 - 1;

    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
    alignas(kCacheLine) std::array<Cell, CapacityPow2> cells_;
};

}  // namespace ghost_story::core
