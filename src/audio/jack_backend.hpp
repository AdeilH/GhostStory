// addu94 — src/audio/jack_backend.hpp
//
// JACK playback backend (spec §3, ADR 002): a jack_client with two output
// ports (addu94:out_L / addu94:out_R). The JACK process callback is the RT
// path: it calls the stored RenderFn DIRECTLY (it is RT-safe by contract —
// see audio/backend.hpp) writing into the port buffers obtained via
// jack_port_get_buffer. NO allocation, no locks, no logging in the
// callback; the only syscalls are the two clock_gettime(CLOCK_MONOTONIC)
// timing reads (vDSO, same measurement budget as the PipeWire backend,
// spec §7.2).
//
// Sample rate and buffer size are owned by the JACK server: we record what
// the server reports (jack_get_sample_rate / jack_get_buffer_size, updated
// through their change callbacks) and never try to force cfg values —
// cfg.sample_rate / cfg.buffer_size are advisory.
//
// Xrun accounting: jack_set_xrun_callback bumps the counter and fires
// StateFn(Xrun) directly. The JACK xrun callback runs on the JACK graph
// (RT) thread; the StateFn contract ("called from a backend thread;
// implementations must marshal to their own threads") makes that legal,
// and there is no separate JACK event thread to marshal through.
//
// Availability probe: linked normally against libjack (PkgConfig::JACK);
// available() probes by attempting jack_client_open with JackNoStartServer
// under a unique name and immediately closing it. This was chosen over a
// dlopen("libjack.so.0") test because a JACK *client* backend is only
// usable with a running server (PipeWire provides one via pipewire-jack):
// the probe answers the actually interesting question — "can addu94 stream
// through JACK right now?" — and open() applies the same check.
#pragma once

#include "audio/backend.hpp"

#include <jack/jack.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace addu94::audio {

class JackBackend final : public AudioBackend {
public:
    JackBackend() = default;
    ~JackBackend() override;

    JackBackend(JackBackend const&) = delete;
    JackBackend& operator=(JackBackend const&) = delete;

    [[nodiscard]] const char* name() const noexcept override {
        return "jack";
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

    /// Probe: open a throwaway JACK client (JackNoStartServer). True only
    /// when a JACK-compatible server (jackd or PipeWire's pipewire-jack)
    /// is reachable.
    [[nodiscard]] bool available() const noexcept override;

private:
    // JACK callbacks (registered in open()).
    static int process_thunk(jack_nframes_t nframes, void* arg) noexcept;
    static int xrun_thunk(void* arg) noexcept;
    static int buffer_size_thunk(jack_nframes_t nframes, void* arg) noexcept;
    static int sample_rate_thunk(std::uint32_t rate, void* arg) noexcept;
    static void shutdown_thunk(void* arg) noexcept;

    /// RT path: fill both port buffers, time it. Returns 0 to JACK.
    int handle_process(jack_nframes_t nframes) noexcept;
    /// RT-side accounting only (no logging here — RT thread).
    void record_xrun() noexcept;
    void fire_state(StreamState s) noexcept;
    /// Non-RT: best-effort jack_connect to system playback ports.
    void connect_playback_ports() noexcept;

    jack_client_t* client_{nullptr};
    jack_port_t* port_l_{nullptr};
    jack_port_t* port_r_{nullptr};
    std::atomic<bool> activated_{false};

    // Callback hooks: written by non-RT threads, read by the RT thread, so
    // they ride atomics. user pointer is published before its function.
    std::atomic<RenderFn> render_fn_{nullptr};
    std::atomic<void*> render_user_{nullptr};
    std::atomic<StateFn> state_fn_{nullptr};
    std::atomic<void*> state_user_{nullptr};

    // RT-written statistics; one cache line each (same layout as the
    // PipeWire backend). Relaxed everywhere (stats only).
    struct Stats {
        alignas(64) std::atomic<std::uint64_t> callbacks{0};
        alignas(64) std::atomic<std::uint64_t> xruns{0};
        alignas(64) std::atomic<std::uint32_t> last_callback_us{0};
        alignas(64) std::atomic<std::uint32_t> max_callback_us{0};
        alignas(64) std::atomic<std::uint32_t> sample_rate{48000};
        alignas(64) std::atomic<std::uint32_t> buffer_size{512};
    };
    Stats stats_{};

    std::atomic<bool> running_{false};       // activated + streaming
    std::atomic<bool> shutdown_seen_{false}; // server told us goodbye
};

}  // namespace addu94::audio
