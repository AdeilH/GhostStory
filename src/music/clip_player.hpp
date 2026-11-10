// ghost_story — src/music/clip_player.hpp
//
// RT clip sequencer: plays one track's recorded clip against the transport
// sample clock. This is the piece that makes "record → press play" actually
// produce sound — until it existed, clip events were only ever read by the
// offline WAV/MIDI exporter, so a finished take was silent in the app.
//
// Threading contract (mirrors the recorder/transport split):
//  - App thread: publish() copies a Clip into a FIXED-CAPACITY snapshot
//    (ticks pre-converted to sample offsets at the current tempo/rate).
//  - RT: render() scans the snapshot wait-free behind a seqlock; no locks,
//    no allocation, no logging (house RT rule, same as theme.hpp's 30 Hz
//    contract but for the audio callback).
//  - A tempo or sample-rate change re-publishes from the app thread; within
//    one buffer the offsets are constant, which is exactly the same
//    "changes land at buffer boundaries" contract TransportControl uses.
//
// The clip loops at its OWN length (loop_samples = length_ticks scaled),
// independent of transport loop_bars: LOOP ∞ (0) keeps the transport
// running forever and the clip keeps re-firing; LOOP N just wraps the
// transport position — events re-fire every clip length either way.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "music/clip.hpp"
#include "music/transport.hpp"  // kPPQ

namespace ghost_story::music {

class ClipPlayer {
public:
    // 8192 events ≈ a very dense 4-bar loop (16th notes × chords × takes
    // still fits several times over). The recorder refuses to exceed this
    // (PlayEngine logs once), so the fixed RT array is bounded by design.
    static constexpr std::size_t kMaxEvents = 8192;

    /// App thread: (re)publish `clip` as this player's playable snapshot.
    /// `bpm`/`sample_rate` convert ticks → sample offsets; call again when
    /// either changes. Bounded copy, sorted insert (clip events are already
    /// tick-sorted, so offsets arrive in order — on-before-off at equal
    /// ticks is preserved by the recorder's insert order).
    void publish(Clip const& clip, double bpm, double sample_rate) noexcept {
        if (bpm < 20.0) bpm = 20.0;
        if (bpm > 333.0) bpm = 333.0;
        double const sec_per_tick = (60.0 / bpm) / static_cast<double>(kPPQ);
        double const ticks_to_samples = sec_per_tick * sample_rate;
        std::uint64_t const loop = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(clip.length_ticks) *
                         ticks_to_samples));

        // Seqlock write phase: readers retry while the counter is odd or
        // moved. WHY acq_rel RMWs and no fences: std::atomic_thread_fence
        // is unsupported under TSan (GCC refuses to instrument it, and the
        // CI builds with -Werror). The release on the closing increment
        // publishes the payload writes to the reader's acquire loads; the
        // opening increment is an RMW the compiler will not hoist payload
        // stores across (the same one-sided-barrier contract kernel
        // seqlocks rely on).
        seq_.fetch_add(1, std::memory_order_acq_rel);  // → odd
        start_offset_ = static_cast<std::uint64_t>(std::llround(
            static_cast<double>(clip.start_tick) * ticks_to_samples));
        std::uint32_t n = 0;
        if (loop > 0) {
            for (auto const& e : clip.events) {
                if (n >= kMaxEvents) break;
                auto const off = static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(e.tick) * ticks_to_samples));
                if (off >= loop) continue;  // event at/past the loop end
                events_[n] = Sched{
                    static_cast<std::uint32_t>(off), e.channel, e.note,
                    e.velocity, e.on};
                ++n;
            }
        }
        count_ = n;
        loop_samples_ = loop;
        seq_.fetch_add(1, std::memory_order_acq_rel);  // → even
        last_bpm_ = bpm;
        last_sr_ = sample_rate;
    }

    [[nodiscard]] double last_bpm() const noexcept { return last_bpm_; }
    [[nodiscard]] double last_sample_rate() const noexcept {
        return last_sr_;
    }
    [[nodiscard]] std::size_t published_count() const noexcept {
        return count_;
    }

    /// RT: fire every event whose loop position lands in
    /// [start_sample, start_sample + frames) into `sink(channel, note,
    /// velocity, on)`. `start_sample` is the transport's absolute sample
    /// position at the START of this buffer (the master hook runs before
    /// Transport::advance). Repeats every loop_samples forever.
    template <typename Sink>
    void render(std::uint64_t start_sample, std::uint32_t frames,
                Sink&& sink) noexcept {
        // Seqlock read: retry while the writer is mid-publish. The scan is
        // short (bounded array, usually near-empty), so a retry costs less
        // than any locking scheme would.
        for (;;) {
            std::uint32_t const s1 = seq_.load(std::memory_order_acquire);
            if ((s1 & 1u) != 0) continue;
            std::uint32_t const count = count_;
            std::uint64_t const loop = loop_samples_;
            if (count == 0 || loop == 0) return;

            // Placement: the clip's local tick 0 sits start_offset samples
            // into the transport timeline — silent before it, looping after.
            if (start_sample < start_offset_) return;
            std::uint64_t const base = (start_sample - start_offset_) % loop;
            scan_window(base, frames, sink);
            if (base + frames > loop) {
                scan_window(0, static_cast<std::uint32_t>(base + frames - loop),
                            sink);
            }

            std::uint32_t const s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) return;  // stable read
            // Writer raced us: fall through and re-scan (fires may have
            // duplicated — harmless: extra note-on on a sounding note is
            // absorbed by the synths, and the app thread publishes at most
            // a few times per second).
        }
    }

    /// RT: note-off every note this player left sounding (transport stop →
    /// tails release naturally instead of hanging). Track which notes are
    /// active per note number — one channel per track's instrument, so a
    /// counter keyed by note is enough.
    template <typename Sink>
    void release_all(Sink&& sink) noexcept {
        for (std::size_t n = 0; n < active_.size(); ++n) {
            if (active_[n] != 0) {
                active_[n] = 0;
                sink(active_channel_, static_cast<int>(n), 0, false);
            }
        }
    }

private:
    struct Sched {
        std::uint32_t offset;  // sample offset within the loop (< loop)
        std::uint8_t channel;
        std::uint8_t note;
        std::uint8_t velocity;
        bool on;
    };

    template <typename Sink>
    void scan_window(std::uint64_t window_start, std::uint32_t frames,
                     Sink&& sink) noexcept {
        std::uint64_t const window_end = window_start + frames;
        for (std::uint32_t i = 0; i < count_ && i < kMaxEvents; ++i) {
            std::uint64_t const off = events_[i].offset;
            if (off < window_start || off >= window_end) continue;
            fire(events_[i], sink);
        }
    }
    // NB: events fire with the buffer boundary as their onset — voices
    // start at the top of the buffer instead of mid-buffer. Same trade the
    // synth_q_ path makes for live notes (≤ one buffer of latency); the
    // offline exporter keeps sub-buffer precision for file renders.

    template <typename Sink>
    void fire(Sched const& e, Sink&& sink) noexcept {
        if (e.on) {
            if (active_[e.note] < 255) ++active_[e.note];
        } else if (active_[e.note] != 0) {
            --active_[e.note];
        }
        sink(e.channel, e.note, e.velocity, e.on);
    }

    std::atomic<std::uint32_t> seq_{0};  // even = stable, odd = publishing
    std::array<Sched, kMaxEvents> events_{};
    std::uint32_t count_{0};
    std::uint64_t loop_samples_{0};
    std::uint64_t start_offset_{0};  // clip placement (start_tick in samples)
    // Per-note sounding count (RT-owned). One instrument channel per track
    // (track_channel), so the key alone identifies the voice pair.
    std::array<std::uint8_t, 128> active_{};
    std::uint8_t active_channel_{0};
    double last_bpm_{0.0};
    double last_sr_{0.0};
};

}  // namespace ghost_story::music
