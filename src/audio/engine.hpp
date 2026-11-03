// addu94 — src/audio/engine.hpp
//
// AudioEngine: owns the backend selection (PipeWire → fallbacks), the
// transport, and the top-level render callback (metronome for M2; mixer +
// instruments land in M3/M6). All RT-callback work is allocation-free.
#pragma once

#include "audio/backend.hpp"
#include "core/spsc_queue.hpp"
#include "music/transport.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace addu94::audio {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(AudioEngine const&) = delete;
    AudioEngine& operator=(AudioEngine const&) = delete;

    /// Start audio with the first working backend. On success, transport
    /// runs in sync with the audio callback.
    bool start(BackendConfig const& cfg, std::string& reason);
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::string backend_name() const;
    [[nodiscard]] CallbackStats stats() const noexcept;

    /// Master render source (M3+): invoked from the RT callback BEFORE the
    /// metronome. The callee fills the buffers additively or exclusively —
    /// the engine zeroes them first. Must be RT-safe; set before start()
    /// or via setter from the control thread while running (pointer swap
    /// is atomic-enough: word-size store).
    void set_master(void (*fn)(Sample*, Sample*, std::uint32_t, void*),
                    void* user) noexcept;

    /// Transport access: control-thread APIs (tempo, meter, loop, metro).
    [[nodiscard]] music::TransportControl& transport_control() noexcept {
        return ctrl_;
    }
    /// RT snapshot of position (relaxed atomic read equivalent — reading
    /// a u64 from another thread is OK for UI display purposes).
    [[nodiscard]] std::uint64_t transport_position() const noexcept;

    /// Called from the RT render callback — for M2: silence + metronome.
    void render(Sample* left, Sample* right, std::uint32_t frames) noexcept;

    /// Bench access: drain RT-pushed callback durations (microseconds).
    /// Only populated when bench_recording_ is on (bench command owns it).
    bool bench_start_record() noexcept;   // returns true if newly on
    bool bench_stop_record() noexcept;
    std::size_t bench_drain(std::uint32_t* out, std::size_t max) noexcept;
private:
    static void render_trampoline(Sample* l, Sample* r, std::uint32_t f,
                                  void* user) noexcept;
    static void state_trampoline(StreamState s, void* user) noexcept;

    music::TransportControl ctrl_;
    music::Transport transport_{ctrl_};
    std::vector<std::unique_ptr<AudioBackend>> backends_;
    AudioBackend* active_{nullptr};
    double sr_{48000.0};  // latched at start (control thread)

    // Master render hook (RT): fn + user. Word-size pointer pair — the RT
    // thread reads both; a torn pair is impossible on x86-64 for aligned
    // pointers, and worst case is one callback with the new fn and old
    // user — prevented by setting user first.
    void (*master_fn_)(Sample*, Sample*, std::uint32_t, void*){nullptr};
    void* master_user_{nullptr};

    // Callback timing → bench (SPSC, RT producer, bench consumer).
    static constexpr std::size_t kTimingCap = 8192;
    core::SpscQueue<std::uint32_t, kTimingCap> timing_q_;
    std::uint32_t timing_scratch_{0};
    std::atomic<bool> bench_recording_{false};
};

}  // namespace addu94::audio
