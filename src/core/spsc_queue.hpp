// addu94 — src/core/spsc_queue.hpp
//
// Wait-free single-producer / single-consumer ring buffer.
// The foundational realtime primitive: audio callback (consumer) and MIDI
// thread (producer) exchange events through this; TSan-tested in CI.
//
// RT-safety contract: push/pop never allocate, lock, or make syscalls and
// never throw (T must be noexcept-movable or trivially copyable).
//
// Layout notes:
//  - capacity must be a power of two; index masking is branch-free.
//  - producer and consumer indices live on separate cache lines to avoid
//    false sharing between the audio and MIDI cores.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace addu94::core {

// Pinned to 64 on purpose (GCC -Winterference-size: the std constant's
// value varies across compiler versions/-mtune, making it unusable in a
// stable ABI; on x86-64 Linux the true destructive interference size is 64).
inline constexpr std::size_t kCacheLine = 64;

/// Bounded SPSC queue. T must be noexcept-movable (or trivially copyable).
template <typename T, std::size_t CapacityPow2>
class SpscQueue {
    static_assert(CapacityPow2 != 0 &&
                      (CapacityPow2 & (CapacityPow2 - 1)) == 0,
                  "CapacityPow2 must be a power of two");
    static_assert(std::is_nothrow_move_constructible_v<T> ||
                      std::is_trivially_copyable_v<T>,
                  "T must be noexcept-movable or trivially copyable");

public:
    constexpr SpscQueue() noexcept = default;
    SpscQueue(SpscQueue const&) = delete;
    SpscQueue& operator=(SpscQueue const&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return CapacityPow2;
    }

    /// Producer side. Wait-free. Returns false when full (element NOT
    /// enqueued). Realtime-safe.
    template <typename... Args>
    [[nodiscard]] bool emplace(Args&&... args) noexcept {
        auto const head = head_.load(std::memory_order_relaxed);
        auto tail = tail_cache_;
        if (head - tail == CapacityPow2) {
            // Cached consumer index may be stale: refresh once.
            tail = tail_.load(std::memory_order_acquire);
            tail_cache_ = tail;
            if (head - tail == CapacityPow2) return false;
        }
        T* slot = slot_ptr(head & kMask);
        ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Producer side convenience: emplace from an existing value.
    [[nodiscard]] bool push(T value) noexcept {
        return emplace(std::move(value));
    }

    /// Consumer side. Wait-free. Returns false when empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        auto const tail = tail_.load(std::memory_order_relaxed);
        auto head = head_cache_;
        if (tail == head) {
            // Cached producer index may be stale: refresh once.
            head = head_.load(std::memory_order_acquire);
            head_cache_ = head;
            if (tail == head) return false;
        }
        T* slot = slot_ptr(tail & kMask);
        out = std::move(*slot);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            slot->~T();
        }
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Approximate element count (relaxed; for stats only, never for
    /// control decisions on the RT path). RT-safe.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return head_.load(std::memory_order_relaxed) -
               tail_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMask = CapacityPow2 - 1;

    [[nodiscard]] T* slot_ptr(std::size_t i) noexcept {
        return std::launder(reinterpret_cast<T*>(raw_.data() + i * sizeof(T)));
    }

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // producer owns
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // consumer owns

    alignas(kCacheLine) std::size_t tail_cache_{0};  // producer's view of tail
    alignas(kCacheLine) std::size_t head_cache_{0};  // consumer's view of head

    alignas(kCacheLine) std::array<std::byte, sizeof(T) * CapacityPow2> raw_{};
};

}  // namespace addu94::core
