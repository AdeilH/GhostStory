// ghost_story — src/audio/pipewire_backend.cpp
#include "audio/pipewire_backend.hpp"

#include "audio/capture_ring.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <ctime>

namespace ghost_story::audio {

namespace {

/// Callback timing: two clock_gettime(CLOCK_MONOTONIC) calls into local
/// timespec structs. CLOCK_MONOTONIC is a vDSO read on Linux (no real
/// syscall, no allocation), keeping the RT callback within the two-call
/// measurement budget of spec §7.2.
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

[[nodiscard]] bool plane_is_usable(struct spa_buffer const* buf,
                                   std::uint32_t index) noexcept {
    return buf != nullptr && buf->n_datas > index &&
           buf->datas[index].data != nullptr &&
           buf->datas[index].chunk != nullptr;
}

}  // namespace

const struct pw_stream_events& PipeWireBackend::stream_events() noexcept {
    static const struct pw_stream_events events = [] {
        struct pw_stream_events ev{};
        ev.version = PW_VERSION_STREAM_EVENTS;
        ev.state_changed = &PipeWireBackend::on_state_changed_thunk;
        ev.process = &PipeWireBackend::on_process_thunk;
        return ev;
    }();
    return events;
}

const struct pw_stream_events&
    PipeWireBackend::capture_stream_events() noexcept {
    static const struct pw_stream_events events = [] {
        struct pw_stream_events ev{};
        ev.version = PW_VERSION_STREAM_EVENTS;
        ev.process = &PipeWireBackend::on_capture_process_thunk;
        return ev;
    }();
    return events;
}

void PipeWireBackend::on_process_thunk(void* arg) noexcept {
    // PipeWire owns this thread — label it so its log lines show "audio".
    ghost_story::log::Logger::set_thread_name("audio");
    static_cast<PipeWireBackend*>(arg)->handle_process();
}

void PipeWireBackend::on_capture_process_thunk(void* arg) noexcept {
    ghost_story::log::Logger::set_thread_name("audio");
    static_cast<PipeWireBackend*>(arg)->handle_capture_process();
}

void PipeWireBackend::on_state_changed_thunk(void* arg,
                                             enum pw_stream_state /*old_state*/,
                                             enum pw_stream_state state,
                                             const char* error) noexcept {
    static_cast<PipeWireBackend*>(arg)->handle_state_changed(state, error);
}

bool PipeWireBackend::available() const noexcept {
    // pw_init(NULL, NULL) is supported (env scanning is skipped); it is
    // reference-counted and pw_deinit is intentionally never called — the
    // backend lives for the whole process (spec: process lifetime).
    static std::once_flag init_once;
    std::call_once(init_once, [] { pw_init(nullptr, nullptr); });
    return true;
}

bool PipeWireBackend::open(BackendConfig const& cfg, std::string& reason) {
    if (stream_ != nullptr) close();
    (void)available();  // ensure pw_init ran before any PipeWire call

    auto* loop = pw_thread_loop_new("ghost_story-audio", nullptr);
    if (loop == nullptr) {
        reason = "pw_thread_loop_new failed";
        log::warn("audio", "open failed: {}", reason);
        return false;
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_NODE_NAME, "ghost_story",             //
        PW_KEY_NODE_DESCRIPTION, "ghost_story",      //
        PW_KEY_MEDIA_TYPE, "Audio",             //
        PW_KEY_MEDIA_CATEGORY, "Playback",      //
        static_cast<const char*>(nullptr));
    if (props == nullptr) {
        reason = "pw_properties_new failed";
        log::warn("audio", "open failed: {}", reason);
        pw_thread_loop_destroy(loop);
        return false;
    }
    // Request a quantum (frames per cycle) when configured; the session
    // manager stays free to adapt it (we read back the negotiated values in
    // the process callback, not here).
    if (cfg.buffer_size != 0 && cfg.sample_rate != 0) {
        pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
                           cfg.buffer_size, cfg.sample_rate);
    }

    // pw_stream_new_simple consumes `props` (frees them on failure too).
    auto* stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                        "ghost_story", props, &stream_events(),
                                        this);
    if (stream == nullptr) {
        reason = "pw_stream_new_simple failed";
        log::warn("audio", "open failed: {}", reason);
        pw_thread_loop_destroy(loop);
        return false;
    }

    // Negotiate planar f32 stereo with fixed FL/FR positions and (when
    // configured) a fixed sample rate. Built once here — no allocation can
    // happen at connect time afterwards. NB: SPA_AUDIO_INFO_RAW_INIT uses a
    // C compound literal (GNU extension, -Wpedantic); the struct is filled
    // by hand instead.
    struct spa_audio_info_raw raw{};
    raw.format = SPA_AUDIO_FORMAT_F32P;
    raw.rate = cfg.sample_rate != 0 ? cfg.sample_rate : 48000;
    raw.channels = 2;
    raw.position[0] = SPA_AUDIO_CHANNEL_FL;
    raw.position[1] = SPA_AUDIO_CHANNEL_FR;

    struct spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer_.data(),
                         static_cast<std::uint32_t>(pod_buffer_.size()));
    params_[0] = spa_format_audio_raw_ext_build(
        &builder, SPA_PARAM_EnumFormat, &raw, sizeof(raw));
    if (params_[0] == nullptr) {
        reason = "spa_format_audio_raw_build failed";
        log::warn("audio", "open failed: {}", reason);
        pw_stream_destroy(stream);
        pw_thread_loop_destroy(loop);
        return false;
    }

    loop_ = loop;
    stream_ = stream;
    cfg_ = cfg;

    // Live mic: a second stream, same loop (single data thread → the
    // capture ring's single-producer guarantee holds). Any failure here
    // downgrades to playback-only with a warning — the mixer must play.
    capture_ring_ = cfg.capture_ring;
    capture_stream_ = nullptr;
    if (capture_ring_ != nullptr) {
        pw_properties* cprops = pw_properties_new(
            PW_KEY_NODE_NAME, "ghost_story-mic",              //
            PW_KEY_NODE_DESCRIPTION, "ghost_story mic",       //
            PW_KEY_MEDIA_TYPE, "Audio",                  //
            PW_KEY_MEDIA_CATEGORY, "Capture",            //
            PW_KEY_MEDIA_ROLE, "Music",                  //
            static_cast<const char*>(nullptr));
        if (cprops != nullptr && cfg.buffer_size != 0 && cfg.sample_rate != 0) {
            pw_properties_setf(cprops, PW_KEY_NODE_LATENCY, "%u/%u",
                               cfg.buffer_size, cfg.sample_rate);
        }
        auto* cstream = cprops == nullptr
                            ? nullptr
                            : pw_stream_new_simple(pw_thread_loop_get_loop(loop),
                                                   "ghost_story-mic", cprops,
                                                   &capture_stream_events(),
                                                   this);
        if (cstream == nullptr) {
            log::warn("audio",
                      "pipewire capture stream creation failed — mic "
                      "disabled (playback continues)");
            capture_ring_ = nullptr;
        } else {
            struct spa_audio_info_raw craw{};
            craw.format = SPA_AUDIO_FORMAT_F32P;
            craw.rate = raw.rate;
            craw.channels = 2;
            craw.position[0] = SPA_AUDIO_CHANNEL_FL;
            craw.position[1] = SPA_AUDIO_CHANNEL_FR;
            struct spa_pod_builder cbuilder{};
            spa_pod_builder_init(&cbuilder, capture_pod_buffer_.data(),
                                 static_cast<std::uint32_t>(
                                     capture_pod_buffer_.size()));
            capture_params_[0] = spa_format_audio_raw_ext_build(
                &cbuilder, SPA_PARAM_EnumFormat, &craw, sizeof(craw));
            if (capture_params_[0] == nullptr) {
                log::warn("audio",
                          "pipewire capture format build failed — mic "
                          "disabled (playback continues)");
                pw_stream_destroy(cstream);
                capture_ring_ = nullptr;
            } else {
                capture_stream_ = cstream;
                log::info("audio",
                          "pipewire capture stream created (auto-link to "
                          "default source on start)");
            }
        }
    }

    stats_.sample_rate.store(raw.rate, std::memory_order_relaxed);
    stats_.buffer_size.store(cfg.buffer_size, std::memory_order_relaxed);
    log::info("audio", "pipewire backend open: rate={} buffer={} (0=device)",
              raw.rate, cfg.buffer_size);
    return true;
}

void PipeWireBackend::close() noexcept {
    if (loop_ != nullptr && stream_ != nullptr && loop_started_.load()) {
        // Destroying the stream from outside the loop requires the lock;
        // the lock must be RELEASED before pw_thread_loop_stop (the loop
        // thread cannot service stop while we hold it — deadlock).
        pw_thread_loop_lock(loop_);
        pw_stream_set_active(stream_, false);
        pw_stream_disconnect(stream_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
        if (capture_stream_ != nullptr) {
            pw_stream_set_active(capture_stream_, false);
            pw_stream_disconnect(capture_stream_);
            pw_stream_destroy(capture_stream_);
            capture_stream_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        loop_started_.store(false);
    } else if (stream_ != nullptr) {
        // Loop never started: no concurrent event dispatch, destroy directly.
        pw_stream_destroy(stream_);
        stream_ = nullptr;
        if (capture_stream_ != nullptr) {
            pw_stream_destroy(capture_stream_);
            capture_stream_ = nullptr;
        }
    }
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    capture_ring_ = nullptr;
    running_.store(false, std::memory_order_release);
    connect_error_.store(false, std::memory_order_relaxed);
    pending_xrun_.store(false, std::memory_order_relaxed);
}

void PipeWireBackend::set_render(RenderFn fn, void* user) noexcept {
    // Publish the user pointer before the function that will receive it.
    render_user_.store(user, std::memory_order_relaxed);
    render_fn_.store(fn, std::memory_order_release);
}

void PipeWireBackend::set_state_fn(StateFn fn, void* user) noexcept {
    state_user_.store(user, std::memory_order_relaxed);
    state_fn_.store(fn, std::memory_order_release);
}

bool PipeWireBackend::start() {
    if (loop_ == nullptr || stream_ == nullptr) {
        log::warn("audio", "start called on closed backend");
        return false;
    }
    if (!loop_started_.exchange(true, std::memory_order_acq_rel)) {
        if (pw_thread_loop_start(loop_) < 0) {
            loop_started_.store(false, std::memory_order_release);
            log::warn("audio", "pw_thread_loop_start failed");
            return false;
        }
    }

    running_.store(false, std::memory_order_relaxed);
    connect_error_.store(false, std::memory_order_relaxed);
    fire_state(StreamState::Starting);

    pw_thread_loop_lock(loop_);
    int const rc = pw_stream_connect(
        stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_RT_PROCESS |
                                          PW_STREAM_FLAG_MAP_BUFFERS |
                                          PW_STREAM_FLAG_AUTOCONNECT),
        params_.data(), static_cast<std::uint32_t>(params_.size()));
    pw_thread_loop_unlock(loop_);
    if (rc < 0) {
        log::warn("audio", "pw_stream_connect failed: {}",
                  spa_strerror(rc));
        return false;
    }

    // Capture stream: connect in the same direction-INPUT pass; failure
    // only disables the mic, never playback (the wait below still keys on
    // the PLAYBACK stream reaching STREAMING).
    if (capture_stream_ != nullptr) {
        pw_thread_loop_lock(loop_);
        int const crc = pw_stream_connect(
            capture_stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_RT_PROCESS |
                                              PW_STREAM_FLAG_MAP_BUFFERS |
                                              PW_STREAM_FLAG_AUTOCONNECT),
            capture_params_.data(),
            static_cast<std::uint32_t>(capture_params_.size()));
        pw_thread_loop_unlock(loop_);
        if (crc < 0) {
            log::warn("audio",
                      "pipewire capture connect failed: {} — mic disabled",
                      spa_strerror(crc));
        }
    }

    // Wait (non-RT thread) for the state hook to report STREAMING or ERROR.
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (running_.load(std::memory_order_acquire)) {
            log::info("audio", "pipewire stream running: rate={} buffer={}",
                      stats_.sample_rate.load(std::memory_order_relaxed),
                      stats_.buffer_size.load(std::memory_order_relaxed));
            return true;
        }
        if (connect_error_.load(std::memory_order_acquire)) {
            log::warn("audio", "pipewire stream entered ERROR while starting");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    log::warn("audio", "pipewire stream did not reach STREAMING within 2 s");
    return false;
}

void PipeWireBackend::stop() noexcept {
    if (loop_ != nullptr && stream_ != nullptr && loop_started_.load()) {
        pw_thread_loop_lock(loop_);
        pw_stream_set_active(stream_, false);
        pw_thread_loop_unlock(loop_);
    }
    running_.store(false, std::memory_order_release);
}

CallbackStats PipeWireBackend::stats() const noexcept {
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

void PipeWireBackend::handle_process() noexcept {
    timespec t0{};
    read_monotonic(&t0);

    struct pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (b == nullptr) {
        // No buffer available: the graph is starving — underrun.
        record_xrun();
        return;
    }
    struct spa_buffer* buf = b->buffer;
    if (!plane_is_usable(buf, 0) || !plane_is_usable(buf, 1)) {
        record_xrun();
        if (buf != nullptr) pw_stream_queue_buffer(stream_, b);
        return;
    }

    auto* left = static_cast<Sample*>(buf->datas[0].data);
    auto* right = static_cast<Sample*>(buf->datas[1].data);
    std::uint32_t const plane_frames =
        static_cast<std::uint32_t>(buf->datas[0].maxsize / sizeof(Sample));

    // Frames the graph asked for (0 = "whatever fits"); clamp to capacity.
    std::uint32_t const wanted = b->requested > UINT32_MAX
                                     ? UINT32_MAX
                                     : static_cast<std::uint32_t>(b->requested);
    std::uint32_t frames = wanted != 0 ? wanted : plane_frames;
    bool const short_fill = frames > plane_frames;
    if (short_fill) frames = plane_frames;

    RenderFn const render = render_fn_.load(std::memory_order_relaxed);
    if (render != nullptr) {
        render(left, right, frames,
               render_user_.load(std::memory_order_relaxed));
    } else {
        // No renderer attached (yet): keep the device fed with silence.
        std::memset(left, 0, frames * sizeof(Sample));
        std::memset(right, 0, frames * sizeof(Sample));
    }

    std::uint32_t const plane_bytes =
        frames * static_cast<std::uint32_t>(sizeof(Sample));
    for (std::uint32_t i = 0; i < 2; ++i) {
        buf->datas[i].chunk->offset = 0;
        buf->datas[i].chunk->size = plane_bytes;
        buf->datas[i].chunk->stride =
            static_cast<std::int32_t>(sizeof(Sample));
    }
    pw_stream_queue_buffer(stream_, b);

    if (short_fill) {
        // Fewer frames than requested were produced (capacity clamp):
        // counted as an xrun per the contract.
        record_xrun();
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
}

void PipeWireBackend::handle_capture_process() noexcept {
    // RT data thread (same loop as playback): pull the captured planes,
    // push into the ring, requeue. No allocation, no locks, no logging.
    struct pw_buffer* b = pw_stream_dequeue_buffer(capture_stream_);
    if (b == nullptr) return;  // graph starving — skip this cycle
    struct spa_buffer* buf = b->buffer;
    // SPA capture contract: the valid samples live at
    // data + chunk->offset (BYTES) .. + chunk->size. Reading from
    // data[0] instead returns stale ring memory — periodic bursts of
    // garbage every graph cycle, i.e. the scratch the mic monitor fed
    // the master. Honor the offset and clamp to what the plane holds.
    if (!plane_is_usable(buf, 0) || buf->datas[0].chunk->offset >
                                        buf->datas[0].maxsize) {
        pw_stream_queue_buffer(capture_stream_, b);
        return;
    }
    auto* plane = static_cast<std::byte*>(buf->datas[0].data) +
                  buf->datas[0].chunk->offset;
    auto* left = reinterpret_cast<Sample*>(plane);
    std::size_t const plane_samples =
        (buf->datas[0].maxsize - buf->datas[0].chunk->offset) /
        sizeof(Sample);
    std::uint32_t const frames = static_cast<std::uint32_t>(
        std::min<std::size_t>(buf->datas[0].chunk->size / sizeof(Sample),
                              plane_samples));
    if (frames > 0) {
        if (plane_is_usable(buf, 1)) {
            auto* right = reinterpret_cast<Sample*>(
                static_cast<std::byte*>(buf->datas[1].data) +
                buf->datas[1].chunk->offset);
            capture_ring_->push(left, right, frames);
        } else {
            // Mono source (laptop mic, mono USB): PipeWire negotiated one
            // channel — duplicate it into both ring planes instead of
            // dropping every cycle (the old both-planes-required check
            // made capture silently dead on mono sources). push() reads
            // the L and R pointers independently, so the same pointer is
            // safe and allocation-free.
            capture_ring_->push(left, left, frames);
        }
    }
    pw_stream_queue_buffer(capture_stream_, b);
}

void PipeWireBackend::record_xrun() noexcept {
    stats_.xruns.fetch_add(1, std::memory_order_relaxed);
    pending_xrun_.store(true, std::memory_order_release);
}

void PipeWireBackend::fire_state(StreamState s) noexcept {
    StateFn const fn = state_fn_.load(std::memory_order_acquire);
    if (fn != nullptr) {
        fn(s, state_user_.load(std::memory_order_acquire));
    }
}

// Runs on the thread-loop (main) thread while the loop holds its internal
// lock — never on the RT data thread. This is where deferred StateFn calls
// are marshalled out of the RT callback.
void PipeWireBackend::handle_state_changed(enum pw_stream_state state,
                                           const char* error) noexcept {
    switch (state) {
    case PW_STREAM_STATE_STREAMING:
        running_.store(true, std::memory_order_release);
        fire_state(StreamState::Running);
        break;
    case PW_STREAM_STATE_ERROR:
        running_.store(false, std::memory_order_release);
        connect_error_.store(true, std::memory_order_release);
        log::error("audio", "pipewire stream error: {}",
                   error != nullptr ? error : "unknown");
        fire_state(StreamState::Error);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_PAUSED:
        running_.store(false, std::memory_order_release);
        break;
    case PW_STREAM_STATE_CONNECTING:
        break;
    }
    // Flush a deferred underrun notification (set by the RT callback).
    if (pending_xrun_.exchange(false, std::memory_order_acq_rel)) {
        fire_state(StreamState::Xrun);
    }
}

PipeWireBackend::~PipeWireBackend() { close(); }

}  // namespace ghost_story::audio
