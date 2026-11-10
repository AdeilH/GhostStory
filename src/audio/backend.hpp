// ghost_story — src/audio/backend.hpp
//
// Audio backend contract (spec §3). The audio callback does NO allocation,
// no locks, no syscalls, no logging (prime directive). Backends drive the
// mixer/render callback and own their native graph.
//
// Backend selection: PipeWire native (default) → JACK → ALSA, overridable
// in config. M2 ships PipeWire; JACK/ALSA fallbacks follow the same
// interface so they are drop-in.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ghost_story::audio {

class CaptureRing;
using Sample = float;

/// Per-callback statistics (written by the RT thread via atomics, read by
/// bench/ops). Non-RT paths only.
struct CallbackStats {
    std::uint64_t callbacks{0};
    std::uint64_t xruns{0};
    std::uint32_t last_callback_us{0};      // duration of last callback
    std::uint32_t max_callback_us{0};       // max since reset
    std::uint32_t sample_rate{48000};
    std::uint32_t buffer_size{512};         // frames per callback
};

enum class StreamState : std::uint8_t {
    Starting,
    Running,
    Xrun,        // recovered glitch; stats updated
    Error,       // unrecoverable; caller should switch backend/repair
};

/// Render callback contract: fill out[0..frames) interleaved? No —
/// planar stereo, channels given. Must be RT-safe (no alloc/lock/log).
using RenderFn = void (*)(Sample* left, Sample* right, std::uint32_t frames,
                          void* user);

/// Notification that the stream died or device changed (called from a
/// backend thread; implementations must marshal to their own threads).
using StateFn = void (*)(StreamState state, void* user);

struct BackendConfig {
    std::uint32_t sample_rate{48000};
    std::uint32_t buffer_size{512};   // frames; 0 = device default
    std::string device;               // empty = default device
    /// Live-input sink (audio/capture_ring.hpp). When non-null, a backend
    /// that CAN capture opens an input path (JACK input ports, a PipeWire
    /// capture stream, an ALSA capture thread) and pushes frames here;
    /// backends without capture support ignore it. The ring is owned by
    /// the caller and must outlive the backend.
    CaptureRing* capture_ring{nullptr};
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    /// Human name for logs/doctor ("pipewire", "jack", "alsa").
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Try to open a playback stream. Returns false (with reason) when the
    /// backend is unavailable — the audio engine then falls back.
    virtual bool open(BackendConfig const& cfg, std::string& reason) = 0;
    virtual void close() noexcept = 0;

    virtual void set_render(RenderFn fn, void* user) noexcept = 0;
    virtual void set_state_fn(StateFn fn, void* user) noexcept = 0;

    /// Start/stop streaming. start() returns false if the stream cannot run.
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;

    [[nodiscard]] virtual bool is_running() const noexcept = 0;
    /// Snapshot copy of stats (relaxed atomics; diagnostics only).
    [[nodiscard]] virtual CallbackStats stats() const noexcept = 0;

    /// Availability probe (backend usable on this machine at all?).
    [[nodiscard]] virtual bool available() const noexcept = 0;
};

/// Factory: returns backends in preference order (PipeWire, JACK, ALSA).
/// Caller owns the pointers.
std::vector<std::unique_ptr<AudioBackend>> make_backends();

}  // namespace ghost_story::audio
