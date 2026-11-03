// addu94 — src/music/transport.hpp
//
// Sample-accurate transport (spec §3): position advances in SAMPLES inside
// the audio callback; all musical quantities derive from it via exact
// integer/64-bit math. Callable from RT (advance/get) and control threads
// (tempo changes take effect at the NEXT buffer boundary, never mid-buffer).
//
// Clock math rules (property-tested in M2 tests):
//  - position is a monotonically increasing sample counter (uint64) —
//    no float drift ever, across arbitrarily many tempo changes.
//  - beat/tick conversion uses 96 PPQ ticks: ticks = position * tempo *
//    PPQ / (sample_rate * 60) computed in 128-bit-safe integers.
//  - loop length in bars wraps position to loop start sample-exactly.
#pragma once

#include <atomic>
#include <cstdint>

namespace addu94::music {

inline constexpr int kPPQ = 96;  // ticks per quarter note (internal grid)

enum class PlayState : std::uint8_t { Stopped, Playing, Recording };

/// Control-plane values (tempo etc.) are written via atomics from non-RT
/// threads and latched at buffer boundaries by the RT thread.
struct TransportControl {
    std::atomic<double> tempo_bpm{120.0};      // 20..333
    std::atomic<std::uint8_t> meter_num{4};    // beats per bar
    std::atomic<std::uint8_t> meter_den{4};    // 4 => quarter
    std::atomic<std::uint64_t> loop_bars{0};   // 0 = no loop
    std::atomic<bool> metronome_on{false};
    std::atomic<float> metronome_gain{0.35f};
    std::atomic<bool> count_in_bars{0};        // 0..4
    // Transport state requests, applied by the RT thread at buffer
    // boundaries (control thread writes, RT consumes-and-clears).
    static constexpr std::uint8_t kReqNone = 0;
    static constexpr std::uint8_t kReqPlay = 1;
    static constexpr std::uint8_t kReqStop = 2;
    static constexpr std::uint8_t kReqToggleRecord = 3;
    std::atomic<std::uint8_t> state_req{kReqNone};
};

class Transport {
public:
    explicit Transport(TransportControl& ctrl) noexcept;

    /// RT: advance by `frames` (one buffer).
    void advance(std::uint32_t frames) noexcept;

    /// RT: consume pending state requests at buffer boundary.
    void apply_state_requests() noexcept;

    /// RT: latch control-plane values (tempo etc.) for this buffer.
    void latch() noexcept;

    /// RT: start/stop — position changes take effect at the next advance.
    void start() noexcept;
    void stop() noexcept;
    void set_recording(bool on) noexcept;

    /// RT reads (no locks):
    [[nodiscard]] std::uint64_t position() const noexcept { return pos_; }
    [[nodiscard]] PlayState state() const noexcept { return state_; }

    /// Musical position helpers (exact):
    /// Quarter notes elapsed as a rational numerator/denominator? We expose
    /// ticks (96 PPQ) computed exactly for the *current* latched tempo.
    [[nodiscard]] double ticks() const noexcept;      // 96 PPQ position
    [[nodiscard]] double beats() const noexcept;      // quarters
    [[nodiscard]] double bars() const noexcept;       // meter bars
    [[nodiscard]] bool on_beat_boundary() const noexcept;

    /// Metronome click scheduling: returns the number of click onsets in
    /// [pos, pos+frames), writing their sample offsets + accent flags.
    /// Clicks land on beat starts (and optionally subdivisions later).
    struct Click { std::uint32_t offset; bool accent; };
    [[nodiscard]] int clicks_in(std::uint32_t frames, Click* out,
                                int max_out) const noexcept;

    /// Seconds per beat (latched tempo).
    [[nodiscard]] double seconds_per_beat() const noexcept {
        return 60.0 / tempo_;
    }

    // Control-thread APIs:
    void set_loop_bars(std::uint64_t bars) noexcept;  // non-RT ok via atomics
    /// Control-thread only, before audio starts.
    void set_sample_rate(std::uint32_t sr) noexcept;

private:
    TransportControl& ctrl_;
    double tempo_{120.0};
    std::uint32_t meter_num_{4};
    std::uint32_t meter_den_{4};
    std::uint64_t loop_samples_{0};

    std::uint64_t pos_{0};
    PlayState state_{PlayState::Stopped};
    double sample_rate_{48000.0};
    std::uint32_t sr_{48000};
};

}  // namespace addu94::music
