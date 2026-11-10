// ghost_story — src/audio/capture_ring.hpp
//
// Lock-free stereo frame rings for the live-input path. Both are SPSC
// (single producer / single consumer) in the core/spsc_queue.hpp house
// style: monotonically increasing u64 cursors masked into a power-of-two
// buffer, cache-line-separated producer/consumer state, and push/pop that
// never allocate, lock, log or make syscalls.
//
// Two policies, because the two hops want opposite overflow behavior:
//
//   CaptureRing  backend → RT render. Capacity is a few hundred ms of
//                buffer. If the render callback stalls (xrun), the freshest
//                input wins: drop NEW frames on full rather than letting
//                the mic fall further behind.
//   HistoryRing  RT render → app grab. A rolling window of the last ~11 s
//                of post-gain mic. When the producer outruns the window the
//                OLDEST frames die (a window that forgets the past keeps
//                the newest seconds grab-able at all times).
//
// Memory ordering contract: the producer publishes frames with
// head_.store(release) AFTER the data writes; the consumer reads the
// tail cursor FIRST (acquire), then head (acquire) — so every snapshot
// satisfies tail ≤ head and copied frames are fully written. The
// HistoryRing producer advances the consumer cursor (overwrite-oldest)
// only within head + n bounds, never past it.
//
// Frame format everywhere: planar float32 stereo, one call per block,
// `n` frames into left[i]/right[i].
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/spsc_queue.hpp"  // kCacheLine

namespace ghost_story::audio {

class CaptureRing {
public:
    /// `capacity_frames` is rounded UP to a power of two; the storage is
    /// allocated here (control thread) so RT push/pop never allocate.
    explicit CaptureRing(std::uint32_t capacity_frames)
        : cap_(round_pow2(capacity_frames)),
          mask_(cap_ - 1),
          left_(std::make_unique<float[]>(cap_)),
          right_(std::make_unique<float[]>(cap_)) {}

    CaptureRing(CaptureRing const&) = delete;
    CaptureRing& operator=(CaptureRing const&) = delete;

    [[nodiscard]] std::uint64_t capacity() const noexcept { return cap_; }

    /// Producer (backend capture thread/callback). Wait-free. Frames that
    /// do not fit are DROPPED (drop-new policy) and counted in dropped().
    void push(float const* l, float const* r, std::uint64_t n) noexcept {
        if (n == 0) return;
        std::uint64_t const head = head_.load(std::memory_order_relaxed);
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (head - tail > cap_ - n) {
            tail = tail_.load(std::memory_order_acquire);  // refresh once
        }
        std::uint64_t const free_ = cap_ - (head - tail);
        if (n > free_) {
            dropped_.fetch_add(n - free_, std::memory_order_relaxed);
            n = free_;
            if (n == 0) return;
        }
        write_at(head, l, r, n);
        head_.store(head + n, std::memory_order_release);
    }

    /// Consumer (RT render callback). Returns frames actually copied
    /// (0 when the ring is empty — the caller treats the shortfall as
    /// silence for that stretch).
    [[nodiscard]] std::uint64_t pop(float* l, float* r,
                                    std::uint64_t max_n) noexcept {
        if (max_n == 0) return 0;
        std::uint64_t const tail = tail_.load(std::memory_order_relaxed);
        std::uint64_t const head = head_.load(std::memory_order_acquire);
        std::uint64_t const avail = head - tail;
        std::uint64_t const n = max_n < avail ? max_n : avail;
        if (n > 0) {
            for (std::uint64_t i = 0; i < n; ++i) {
                l[i] = left_[(tail + i) & mask_];
                r[i] = right_[(tail + i) & mask_];
            }
            tail_.store(tail + n, std::memory_order_release);
        }
        return n;
    }

    /// Diagnostic counter of frames lost to overflow (relaxed).
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static std::uint64_t round_pow2(std::uint64_t v) noexcept {
        std::uint64_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void write_at(std::uint64_t pos, float const* l, float const* r,
                  std::uint64_t n) noexcept {
        for (std::uint64_t i = 0; i < n; ++i) {
            left_[(pos + i) & mask_] = l[i];
            right_[(pos + i) & mask_] = r[i];
        }
    }

    std::uint64_t cap_;
    std::uint64_t mask_;  // cap_ - 1
    alignas(core::kCacheLine) std::atomic<std::uint64_t> head_{0};
    alignas(core::kCacheLine) std::atomic<std::uint64_t> tail_{0};
    alignas(core::kCacheLine) std::atomic<std::uint64_t> dropped_{0};
    std::unique_ptr<float[]> left_;
    std::unique_ptr<float[]> right_;
};

class HistoryRing {
public:
    /// `capacity_frames` is rounded UP to a power of two (2^19 ≈ 11 s at
    /// 48 kHz); the storage is allocated here (control thread).
    explicit HistoryRing(std::uint32_t capacity_frames)
        : cap_(round_pow2(capacity_frames)),
          mask_(cap_ - 1),
          left_(std::make_unique<float[]>(cap_)),
          right_(std::make_unique<float[]>(cap_)) {}

    HistoryRing(HistoryRing const&) = delete;
    HistoryRing& operator=(HistoryRing const&) = delete;

    [[nodiscard]] std::uint64_t capacity() const noexcept { return cap_; }

    /// Producer (RT render callback). Wait-free, ALWAYS succeeds: when the
    /// push exceeds the window, the oldest frames are overwritten.
    /// NOTE: pushes of n >= cap_ rewrite the whole window and are only
    /// valid when the consumer is quiescent (the mic path never does this:
    /// render blocks are ≤ 8192 frames ≪ cap_).
    void push(float const* l, float const* r, std::uint64_t n) noexcept {
        if (n == 0) return;
        std::uint64_t const head = head_.load(std::memory_order_relaxed);
        if (n >= cap_) {
            // Longer than the whole window: keep only the newest cap_.
            std::uint64_t const skip = n - cap_;
            write_at(head, l + skip, r + skip, cap_);
            head_.store(head + cap_, std::memory_order_release);
            tail_.store(head, std::memory_order_release);
            return;
        }
        std::uint64_t const tail = tail_.load(std::memory_order_relaxed);
        if (head - tail + n > cap_) {
            // Overwrite-oldest: the consumer cursor dies under the writer
            // so head never laps it (head + n - cap_ < head because
            // n < cap_ — the cursor can never pass the producer).
            tail_.store(head + n - cap_, std::memory_order_release);
        }
        write_at(head, l, r, n);
        head_.store(head + n, std::memory_order_release);
    }

    /// Consumer (app grab). Copies the NEWEST `n` frames, oldest-first
    /// (out[0] is the earliest frame of the returned window). Returns the
    /// frames actually available (0 until the window has data). A push
    /// concurrent with the copy only makes the result slightly stale —
    /// never torn, because copied slots sit strictly below the producer's
    /// cursor.
    [[nodiscard]] std::uint64_t read_last(float* l, float* r,
                                          std::uint64_t n) const noexcept {
        // Tail first, then head (see the ordering contract above).
        std::uint64_t const tail = tail_.load(std::memory_order_acquire);
        std::uint64_t const head = head_.load(std::memory_order_acquire);
        std::uint64_t const used = head - tail;
        if (used == 0) return 0;
        std::uint64_t const start = n < used ? head - n : tail;
        std::uint64_t const count = head - start;
        for (std::uint64_t i = 0; i < count; ++i) {
            l[i] = left_[(start + i) & mask_];
            r[i] = right_[(start + i) & mask_];
        }
        return count;
    }

private:
    [[nodiscard]] static std::uint64_t round_pow2(std::uint64_t v) noexcept {
        std::uint64_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void write_at(std::uint64_t pos, float const* l, float const* r,
                  std::uint64_t n) noexcept {
        for (std::uint64_t i = 0; i < n; ++i) {
            left_[(pos + i) & mask_] = l[i];
            right_[(pos + i) & mask_] = r[i];
        }
    }

    std::uint64_t cap_;
    std::uint64_t mask_;  // cap_ - 1
    alignas(core::kCacheLine) std::atomic<std::uint64_t> head_{0};
    alignas(core::kCacheLine) std::atomic<std::uint64_t> tail_{0};
    std::unique_ptr<float[]> left_;
    std::unique_ptr<float[]> right_;
};

}  // namespace ghost_story::audio
