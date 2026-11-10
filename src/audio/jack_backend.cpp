// ghost_story — src/audio/jack_backend.cpp
#include "audio/jack_backend.hpp"

#include "audio/capture_ring.hpp"
#include "log/logger.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>

namespace ghost_story::audio {

namespace {

/// Callback timing: two clock_gettime(CLOCK_MONOTONIC) calls into local
/// timespec structs (vDSO read on Linux — no real syscall, no allocation).
/// Mirrors the PipeWire backend's measurement so stats are comparable.
void read_monotonic(timespec* ts) noexcept {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

[[nodiscard]] std::uint32_t elapsed_us(timespec const& start,
                                       timespec const& end) noexcept {
    auto const ds = static_cast<std::int64_t>(end.tv_sec) -
                    static_cast<std::int64_t>(start.tv_sec);
    auto const dns = static_cast<std::int64_t>(end.tv_nsec) -
                     static_cast<std::int64_t>(start.tv_nsec);
    std::int64_t const ns = ds * 1'000'000'000 + dns;
    if (ns <= 0) return 0;
    std::int64_t const us = ns / 1'000;
    return us > static_cast<std::int64_t>(UINT32_MAX)
               ? UINT32_MAX
               : static_cast<std::uint32_t>(us);
}

}  // namespace

JackBackend::~JackBackend() { close(); }

bool JackBackend::available() const noexcept {
    // Probe strategy (documented in the header): attempt a real client open
    // with JackNoStartServer. Returns false both when libjack is absent
    // (would have failed to link / dlopen) and when no server is running.
    jack_status_t status = static_cast<jack_status_t>(0);
    jack_client_t* probe =
        jack_client_open("ghost_story-probe", JackNoStartServer, &status);
    if (probe == nullptr) return false;
    jack_client_close(probe);
    return true;
}

bool JackBackend::open(BackendConfig const& cfg, std::string& reason) {
    if (client_ != nullptr) close();

    jack_status_t status = static_cast<jack_status_t>(0);
    // JackNoStartServer: never spawn a server as a side effect of open().
    // On this machine PipeWire exposes a JACK-compatible server via
    // pipewire-jack, so this connects live; without one it fails cleanly.
    jack_client_t* client =
        jack_client_open("ghost_story", JackNoStartServer, &status);
    if (client == nullptr) {
        reason = "jack_client_open failed (status=0x" +
                 std::to_string(static_cast<unsigned>(status)) +
                 ") — no JACK server reachable";
        log::warn("audio", "jack open failed: {}", reason);
        return false;
    }

    // Record what the SERVER gives; cfg values are advisory only.
    auto const server_rate =
        static_cast<std::uint32_t>(jack_get_sample_rate(client));
    auto const server_buffer =
        static_cast<std::uint32_t>(jack_get_buffer_size(client));

    jack_port_t* port_l =
        jack_port_register(client, "out_L", JACK_DEFAULT_AUDIO_TYPE,
                           JackPortIsOutput, 0);
    jack_port_t* port_r =
        jack_port_register(client, "out_R", JACK_DEFAULT_AUDIO_TYPE,
                           JackPortIsOutput, 0);
    if (port_l == nullptr || port_r == nullptr) {
        reason = "jack_port_register(out_L/out_R) failed";
        log::warn("audio", "jack open failed: {}", reason);
        jack_client_close(client);
        return false;
    }

    // Callbacks must be registered before jack_activate().
    jack_set_process_callback(client, &JackBackend::process_thunk, this);
    jack_set_xrun_callback(client, &JackBackend::xrun_thunk, this);
    jack_set_buffer_size_callback(client, &JackBackend::buffer_size_thunk,
                                  this);
    jack_set_sample_rate_callback(client, &JackBackend::sample_rate_thunk,
                                  this);
    jack_on_shutdown(client, &JackBackend::shutdown_thunk, this);

    client_ = client;
    port_l_ = port_l;
    port_r_ = port_r;
    capture_ring_ = cfg.capture_ring;
    in_l_ = nullptr;
    in_r_ = nullptr;
    if (capture_ring_ != nullptr) {
        // Live mic: two input ports. Registration failure downgrades to
        // playback-only (a warning, never fatal — the mixer must play).
        in_l_ = jack_port_register(client, "in_L", JACK_DEFAULT_AUDIO_TYPE,
                                   JackPortIsInput, 0);
        in_r_ = jack_port_register(client, "in_R", JACK_DEFAULT_AUDIO_TYPE,
                                   JackPortIsInput, 0);
        if (in_l_ == nullptr || in_r_ == nullptr) {
            log::warn("audio",
                      "jack: input port registration failed — capture "
                      "disabled (playback continues)");
            in_l_ = nullptr;
            in_r_ = nullptr;
            capture_ring_ = nullptr;
        }
    }
    shutdown_seen_.store(false, std::memory_order_relaxed);
    activated_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    stats_.sample_rate.store(server_rate, std::memory_order_relaxed);
    stats_.buffer_size.store(server_buffer, std::memory_order_relaxed);
    log::info("audio",
              "jack backend open: server rate={} buffer={} "
              "(cfg was rate={} buffer={})",
              server_rate, server_buffer, cfg.sample_rate, cfg.buffer_size);
    return true;
}

void JackBackend::close() noexcept {
    if (client_ == nullptr) return;
    if (activated_.load(std::memory_order_acquire)) {
        jack_deactivate(client_);
        activated_.store(false, std::memory_order_release);
    }
    // jack_client_close deactivates if needed and frees ports; it must not
    // be called from callbacks — we are always on a foreign thread here.
    jack_client_close(client_);
    client_ = nullptr;
    port_l_ = nullptr;
    port_r_ = nullptr;
    in_l_ = nullptr;
    in_r_ = nullptr;
    capture_ring_ = nullptr;
    running_.store(false, std::memory_order_release);
}

void JackBackend::set_render(RenderFn fn, void* user) noexcept {
    // Publish the user pointer before the function that will receive it.
    render_user_.store(user, std::memory_order_relaxed);
    render_fn_.store(fn, std::memory_order_release);
}

void JackBackend::set_state_fn(StateFn fn, void* user) noexcept {
    state_user_.store(user, std::memory_order_relaxed);
    state_fn_.store(fn, std::memory_order_release);
}

bool JackBackend::start() {
    if (client_ == nullptr) {
        log::warn("audio", "jack start called on closed backend");
        return false;
    }
    if (shutdown_seen_.load(std::memory_order_acquire)) {
        log::warn("audio", "jack start after server shutdown");
        return false;
    }

    running_.store(false, std::memory_order_relaxed);
    fire_state(StreamState::Starting);

    if (jack_activate(client_) != 0) {
        log::warn("audio", "jack_activate failed");
        return false;
    }
    activated_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    fire_state(StreamState::Running);

    // Best-effort auto-connect; failure is a warning, never fatal — the
    // session graph may not have system playback ports (or a patchbay may
    // want to route us manually).
    connect_playback_ports();
    if (in_l_ != nullptr && in_r_ != nullptr) {
        connect_capture_ports();
    }

    log::info("audio", "jack backend running: rate={} buffer={}",
              stats_.sample_rate.load(std::memory_order_relaxed),
              stats_.buffer_size.load(std::memory_order_relaxed));
    return true;
}

void JackBackend::stop() noexcept {
    if (client_ != nullptr &&
        activated_.exchange(false, std::memory_order_acq_rel)) {
        jack_deactivate(client_);
    }
    running_.store(false, std::memory_order_release);
}

CallbackStats JackBackend::stats() const noexcept {
    CallbackStats out;
    out.callbacks = stats_.callbacks.load(std::memory_order_relaxed);
    out.xruns = stats_.xruns.load(std::memory_order_relaxed);
    out.last_callback_us =
        stats_.last_callback_us.load(std::memory_order_relaxed);
    out.max_callback_us = stats_.max_callback_us.load(std::memory_order_relaxed);
    out.sample_rate = stats_.sample_rate.load(std::memory_order_relaxed);
    out.buffer_size = stats_.buffer_size.load(std::memory_order_relaxed);
    return out;
}

int JackBackend::process_thunk(jack_nframes_t nframes, void* arg) noexcept {
    // JACK owns this thread — label it so its log lines show "audio".
    ghost_story::log::Logger::set_thread_name("audio");
    return static_cast<JackBackend*>(arg)->handle_process(nframes);
}

int JackBackend::handle_process(jack_nframes_t nframes) noexcept {
    timespec t0{};
    read_monotonic(&t0);

    auto* left = static_cast<Sample*>(jack_port_get_buffer(port_l_, nframes));
    auto* right = static_cast<Sample*>(jack_port_get_buffer(port_r_, nframes));
    if (left == nullptr || right == nullptr) {
        // No port buffers (port unregistered / server shutting down):
        // count as an xrun, keep the callback return value non-fatal.
        record_xrun();
        return 0;
    }

    // Live input: push the capture port buffers into the ring BEFORE the
    // render call (the mixer consumes them in the same graph cycle — input
    // and output move in lockstep, which keeps mic latency at one buffer).
    if (in_l_ != nullptr && in_r_ != nullptr) {
        auto* in_left =
            static_cast<Sample*>(jack_port_get_buffer(in_l_, nframes));
        auto* in_right =
            static_cast<Sample*>(jack_port_get_buffer(in_r_, nframes));
        if (in_left != nullptr && in_right != nullptr) {
            capture_ring_->push(in_left, in_right, nframes);
        }
    }

    RenderFn const render = render_fn_.load(std::memory_order_relaxed);
    if (render != nullptr) {
        render(left, right, nframes,
               render_user_.load(std::memory_order_relaxed));
    } else {
        // No renderer attached (yet): keep the device fed with silence.
        std::memset(left, 0, static_cast<std::size_t>(nframes) * sizeof(Sample));
        std::memset(right, 0, static_cast<std::size_t>(nframes) * sizeof(Sample));
    }

    timespec t1{};
    read_monotonic(&t1);
    std::uint32_t const us = elapsed_us(t0, t1);
    stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
    stats_.last_callback_us.store(us, std::memory_order_relaxed);
    std::uint32_t prev_max = stats_.max_callback_us.load(std::memory_order_relaxed);
    while (us > prev_max &&
           !stats_.max_callback_us.compare_exchange_weak(
               prev_max, us, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    return 0;
}

int JackBackend::xrun_thunk(void* arg) noexcept {
    // Runs on the JACK graph (RT) thread: RT-safe accounting only, then the
    // StateFn(Xrun) the contract permits us to fire from a backend thread.
    auto* self = static_cast<JackBackend*>(arg);
    self->record_xrun();
    self->fire_state(StreamState::Xrun);
    return 0;
}

int JackBackend::buffer_size_thunk(jack_nframes_t nframes, void* arg) noexcept {
    // Server changed its buffer size: record it (we never force one).
    auto* self = static_cast<JackBackend*>(arg);
    self->stats_.buffer_size.store(static_cast<std::uint32_t>(nframes),
                                   std::memory_order_relaxed);
    return 0;
}

int JackBackend::sample_rate_thunk(std::uint32_t rate, void* arg) noexcept {
    auto* self = static_cast<JackBackend*>(arg);
    self->stats_.sample_rate.store(rate, std::memory_order_relaxed);
    return 0;
}

void JackBackend::shutdown_thunk(void* arg) noexcept {
    // The client is dead after this — do NOT call any JACK functions here.
    // Not the RT thread; a log line is acceptable and useful.
    auto* self = static_cast<JackBackend*>(arg);
    self->shutdown_seen_.store(true, std::memory_order_release);
    self->activated_.store(false, std::memory_order_release);
    self->running_.store(false, std::memory_order_release);
    log::warn("audio", "jack server shut down — client is dead");
    self->fire_state(StreamState::Error);
}

void JackBackend::record_xrun() noexcept {
    stats_.xruns.fetch_add(1, std::memory_order_relaxed);
}

void JackBackend::fire_state(StreamState s) noexcept {
    StateFn const fn = state_fn_.load(std::memory_order_acquire);
    if (fn != nullptr) {
        fn(s, state_user_.load(std::memory_order_acquire));
    }
}

void JackBackend::connect_playback_ports() noexcept {
    if (port_l_ == nullptr || port_r_ == nullptr) return;
    char const* src_l = jack_port_name(port_l_);
    char const* src_r = jack_port_name(port_r_);

    // Fast path: standard system playback ports (jackd and PipeWire's
    // pipewire-jack both expose these).
    if (jack_connect(client_, src_l, "system:playback_1") == 0 &&
        jack_connect(client_, src_r, "system:playback_2") == 0) {
        return;
    }

    // Fallback: enumerate input ports and connect to the first two
    // system/physical playback inputs we can find.
    char const** ports = jack_get_ports(
        client_, "system:", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    if (ports == nullptr) {
        log::warn("audio",
                  "jack: no system playback ports found — leaving {} / {} "
                  "unconnected (route manually if needed)",
                  src_l, src_r);
        return;
    }
    std::size_t connected = 0;
    for (char const** p = ports; *p != nullptr && connected < 2; ++p) {
        char const* dst = *p;
        // jack_connect returns 0 on success, EEXIST when already wired.
        int const rc = jack_connect(client_,
                                    connected == 0 ? src_l : src_r, dst);
        if (rc == 0 || rc == EEXIST) {
            ++connected;
        }
    }
    jack_free(ports);
    if (connected < 2) {
        log::warn("audio",
                  "jack: only {} of 2 playback connections succeeded "
                  "(best-effort, non-fatal)",
                  connected);
    }
}

void JackBackend::connect_capture_ports() noexcept {
    if (in_l_ == nullptr || in_r_ == nullptr) return;
    char const* dst_l = jack_port_name(in_l_);
    char const* dst_r = jack_port_name(in_r_);

    // Fast path: the standard system capture ports.
    if (jack_connect(client_, "system:capture_1", dst_l) == 0 &&
        jack_connect(client_, "system:capture_2", dst_r) == 0) {
        log::info("audio", "jack capture: system:capture_1/2 -> {} / {}",
                  dst_l, dst_r);
        return;
    }
    // Fallback: the first physical output ports we can find (a capture
    // device with a single mono mic still lights up the mic panel).
    char const** ports = jack_get_ports(
        client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
        static_cast<JackPortFlags>(JackPortIsPhysical | JackPortIsOutput));
    if (ports == nullptr) {
        log::warn("audio",
                  "jack capture: no system capture ports found — mic stays "
                  "unconnected (route a source to {} manually)",
                  dst_l);
        return;
    }
    std::size_t connected = 0;
    for (char const** p = ports; *p != nullptr && connected < 2; ++p) {
        char const* src = *p;
        int const rc = jack_connect(client_, src,
                                    connected == 0 ? dst_l : dst_r);
        if (rc == 0 || rc == EEXIST) {
            ++connected;
        }
    }
    jack_free(ports);
    if (connected > 0) {
        log::info("audio", "jack capture: {} physical source(s) connected",
                  connected);
    } else {
        log::warn("audio",
                  "jack capture: no capture source connected (non-fatal)");
    }
}

}  // namespace ghost_story::audio
