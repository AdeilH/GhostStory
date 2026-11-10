// ghost_story — src/audio/alsa_backend.hpp
//
// ALSA PCM playback backend (spec §3, ADR 002) — the LAST-RESORT backend,
// used only when neither PipeWire nor a JACK-compatible server is
// reachable. Its latency characteristics differ fundamentally from the
// other backends: ALSA has no callback API here, so a dedicated WRITER
// thread (NOT a realtime callback) drives the stream:
//
//   loop { RenderFn(planar) → interleave → snd_pcm_writei }
//
// The writer thread is ALLOWED to block: snd_pcm_writei sleeps when the
// device buffer is full and blocks up to the buffer duration. There is no
// RT guarantee for the render call, so worst-case callback jitter is
// bounded by the writer thread's scheduling, not by a server graph. All
// buffers (planar pair + interleaved staging) are preallocated in open();
// the thread is started in start() and joined in stop(), so no allocation
// happens on the streaming path.
//
// Underrun handling: -EPIPE counts an xrun, fires StateFn(Xrun) and
// snd_pcm_prepare()s the device (ALSA auto-recovers on the next writei);
// -ESTRPIPE is resumed; any other error stops the thread and fires
// StateFn(Error).
#pragma once

#include "audio/backend.hpp"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ghost_story::audio {

class AlsaBackend final : public AudioBackend {
public:
    AlsaBackend() = default;
    ~AlsaBackend() override;

    AlsaBackend(AlsaBackend const&) = delete;
    AlsaBackend& operator=(AlsaBackend const&) = delete;

    [[nodiscard]] const char* name() const noexcept override {
        return "alsa";
    }

    bool open(BackendConfig const& cfg, std::string& reason) override;
    void close() noexcept override;

    void set_render(RenderFn fn, void* user) noexcept override;
    void set_state_fn(StateFn fn, void* user) noexcept override;

    bool start() override;
    void stop() noexcept override;

    [[nodiscard]] bool is_running() const noexcept override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] CallbackStats stats() const noexcept override;

    /// Probe: name any PCM via snd_device_name_hint. If the hint system
    /// itself errors we return true and let open() give the real answer
    /// (a bare ALSA install always has at least the "default" PCM).
    [[nodiscard]] bool available() const noexcept override;

private:
    /// Writer thread body: render → interleave → writei loop.
    void writer_loop() noexcept;
    /// Capture thread body (live mic, when open() configured one):
    /// snd_pcm_readi → convert → push into the capture ring. Blocking is
    /// expected (same non-RT contract as the writer thread); any capture
    /// failure ends the thread and the mic goes quiet — playback continues.
    void capture_loop() noexcept;
    /// RT-ish timing/accounting helpers (writer thread only).
    void record_xrun() noexcept;
    void fire_state(StreamState s) noexcept;
    /// Fatal path from the writer thread: flag, notify, leave.
    void enter_error(char const* what, int err) noexcept;

    snd_pcm_t* pcm_{nullptr};

    // Capture path (null / empty = playback-only).
    snd_pcm_t* cap_pcm_{nullptr};
    CaptureRing* capture_ring_{nullptr};
    bool cap_s16_{false};  // device refused FLOAT_LE; convert in the loop
    std::vector<Sample> cap_left_;
    std::vector<Sample> cap_right_;
    // Raw interleaved staging: worst case (2 ch × 4-byte float) per frame.
    std::vector<std::byte> cap_raw_;
    std::uint32_t cap_frames_{0};

    // Preallocated in open() — period_frames_ floats each.
    std::vector<Sample> left_;
    std::vector<Sample> right_;
    std::vector<Sample> interleaved_;
    std::uint32_t period_frames_{512};

    // Hook pairs; the writer thread is the only reader after start().
    std::atomic<RenderFn> render_fn_{nullptr};
    std::atomic<void*> render_user_{nullptr};
    std::atomic<StateFn> state_fn_{nullptr};
    std::atomic<void*> state_user_{nullptr};

    // Statistics (writer thread writes, diagnostics read; relaxed).
    struct Stats {
        alignas(64) std::atomic<std::uint64_t> callbacks{0};
        alignas(64) std::atomic<std::uint64_t> xruns{0};
        alignas(64) std::atomic<std::uint32_t> last_callback_us{0};
        alignas(64) std::atomic<std::uint32_t> max_callback_us{0};
        alignas(64) std::atomic<std::uint32_t> sample_rate{48000};
        alignas(64) std::atomic<std::uint32_t> buffer_size{512};
    };
    Stats stats_{};

    std::thread writer_;
    std::thread capturer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> error_{false};
};

}  // namespace ghost_story::audio
