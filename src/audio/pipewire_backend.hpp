// ghost_story — src/audio/pipewire_backend.hpp
//
// PipeWire-native playback backend (spec §3): a pw_thread_loop + pw_stream
// negotiating planar f32 stereo (FL/FR). The on_process callback is the RT
// path: it calls the stored RenderFn directly with NO allocation, no locks,
// no logging and no syscalls beyond the two clock_gettime(CLOCK_MONOTONIC)
// timing calls (vDSO read of a local struct — spec §7.2 permits this for
// callback measurement; steady_clock::now() would be equivalent but
// clock_gettime is used so the "two syscalls max" budget is explicit).
// Everything else is pre-allocated in open().
//
// Xrun/StateFn marshalling: the RT on_process only bumps the xruns counter
// and sets a pending flag. StateFn(Xrun/Error) is fired from the stream's
// state_changed hook, which PipeWire invokes on the thread-loop (main)
// thread — never from the RT callback. pw_loop_invoke is deliberately NOT
// used from on_process (it allocates its invoke item). Consequence: a
// transient underrun that does not change stream state updates the xruns
// counter (visible via stats()) but defers the StateFn(Xrun) notification
// to the next state transition.
//
// Note: state_changed runs while the thread loop holds its internal lock,
// so StateFn implementations must not synchronously call back into this
// backend (per the AudioBackend contract they marshal to their own thread).
#pragma once

#include "audio/backend.hpp"

#include <pipewire/pipewire.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ghost_story::audio {

class PipeWireBackend final : public AudioBackend {
public:
    PipeWireBackend() = default;
    ~PipeWireBackend() override;

    PipeWireBackend(PipeWireBackend const&) = delete;
    PipeWireBackend& operator=(PipeWireBackend const&) = delete;

    [[nodiscard]] const char* name() const noexcept override {
        return "pipewire";
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

    /// Lazily pw_init()s once (process lifetime; pw_deinit is never called)
    /// and reports the native backend usable.
    [[nodiscard]] bool available() const noexcept override;

private:
    // pw_stream event thunks (registered in stream_events()).
    static void on_process_thunk(void* arg) noexcept;
    static void on_state_changed_thunk(void* arg,
                                       enum pw_stream_state old_state,
                                       enum pw_stream_state state,
                                       const char* error) noexcept;
    // Capture stream (live mic): its own events table — only `process`
    // matters; state churn of the input stream never gates playback.
    static void on_capture_process_thunk(void* arg) noexcept;
    static const struct pw_stream_events& capture_stream_events() noexcept;

    static const struct pw_stream_events& stream_events() noexcept;

    /// RT data thread: fill the dequeued buffer's planes, requeue, time it.
    void handle_process() noexcept;
    /// RT data thread: push the dequeued capture planes into the ring,
    /// requeue. Both streams run on the SAME pw_thread_loop, so the ring's
    /// single-producer guarantee holds.
    void handle_capture_process() noexcept;
    /// Thread-loop thread: mirror state, marshal deferred Xrun/Error.
    void handle_state_changed(enum pw_stream_state state,
                              const char* error) noexcept;
    /// RT-safe xrun accounting only (no user callbacks here).
    void record_xrun() noexcept;
    void fire_state(StreamState s) noexcept;

    static constexpr std::size_t kPodBufferSize = 512;

    pw_thread_loop* loop_{nullptr};
    pw_stream* stream_{nullptr};
    // Capture stream — created only when cfg.capture_ring was provided;
    // null = playback-only (capture setup failures downgrade, never fatal).
    pw_stream* capture_stream_{nullptr};
    CaptureRing* capture_ring_{nullptr};

    // SPA EnumFormat pods, built once in open() and handed to
    // pw_stream_connect() on every start().
    std::array<std::uint8_t, kPodBufferSize> pod_buffer_{};
    std::array<const struct spa_pod*, 1> params_{};
    std::array<std::uint8_t, kPodBufferSize> capture_pod_buffer_{};
    std::array<const struct spa_pod*, 1> capture_params_{};
    BackendConfig cfg_{};

    // Callback hooks: written by non-RT threads, read by the RT thread, so
    // they ride atomics. user pointer is published before its function.
    std::atomic<RenderFn> render_fn_{nullptr};
    std::atomic<void*> render_user_{nullptr};
    std::atomic<StateFn> state_fn_{nullptr};
    std::atomic<void*> state_user_{nullptr};

    // RT-written statistics; one cache line each so the audio core does not
    // bounce lines with diagnostics readers. Relaxed everywhere (stats only).
    struct Stats {
        alignas(64) std::atomic<std::uint64_t> callbacks{0};
        alignas(64) std::atomic<std::uint64_t> xruns{0};
        alignas(64) std::atomic<std::uint32_t> last_callback_us{0};
        alignas(64) std::atomic<std::uint32_t> max_callback_us{0};
        alignas(64) std::atomic<std::uint32_t> sample_rate{48000};
        alignas(64) std::atomic<std::uint32_t> buffer_size{512};
    };
    Stats stats_{};

    std::atomic<bool> running_{false};        // stream reached STREAMING
    std::atomic<bool> connect_error_{false};  // stream hit ERROR state
    std::atomic<bool> pending_xrun_{false};   // RT saw an xrun, marshal later
    std::atomic<bool> loop_started_{false};
};

}  // namespace ghost_story::audio
