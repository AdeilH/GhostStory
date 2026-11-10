// ghost_story — src/audio/alsa_backend.cpp
#include "audio/alsa_backend.hpp"

#include "audio/capture_ring.hpp"
#include "log/logger.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

namespace ghost_story::audio {

namespace {

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

AlsaBackend::~AlsaBackend() { close(); }

bool AlsaBackend::available() const noexcept {
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        // Cannot enumerate (restricted /proc, weird ALSA config): assume
        // usable — open() performs the authoritative check anyway.
        return true;
    }
    bool found = false;
    for (void** h = hints; *h != nullptr; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        if (name != nullptr) {
            found = true;
            free(name);  // hint strings are malloc'd by libasound
            if (found) break;  // one playable PCM name is enough
        }
    }
    snd_device_name_free_hint(hints);
    return found;
}

bool AlsaBackend::open(BackendConfig const& cfg, std::string& reason) {
    if (pcm_ != nullptr) close();

    char const* dev = cfg.device.empty() ? "default" : cfg.device.c_str();
    snd_pcm_t* pcm = nullptr;
    int rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        reason = std::string("snd_pcm_open(") + dev + "): " +
                 snd_strerror(rc);
        log::warn("audio", "alsa open failed: {}", reason);
        return false;
    }

    // --- Hardware parameters -------------------------------------------
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_malloc(&hw);
    if (hw == nullptr) {
        reason = "snd_pcm_hw_params_malloc failed";
        log::warn("audio", "alsa open failed: {}", reason);
        snd_pcm_close(pcm);
        return false;
    }

    auto fail = [&](int err, char const* what) {
        reason = std::string(what) + ": " + snd_strerror(err);
        log::warn("audio", "alsa open failed: {}", reason);
        snd_pcm_hw_params_free(hw);
        snd_pcm_close(pcm);
        return false;
    };

    rc = snd_pcm_hw_params_any(pcm, hw);
    if (rc < 0) return fail(rc, "snd_pcm_hw_params_any");
    rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) return fail(rc, "set_access(RW_INTERLEAVED)");
    rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
    if (rc < 0) return fail(rc, "set_format(FLOAT_LE)");
    rc = snd_pcm_hw_params_set_channels(pcm, hw, 2);
    if (rc < 0) return fail(rc, "set_channels(2)");

    // Sample rate: set_rate_near allows resampling by the ALSA plug layer.
    unsigned int rate = cfg.sample_rate != 0 ? cfg.sample_rate : 48000;
    unsigned int approx_rate = rate;
    int dir = 0;
    rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &approx_rate, &dir);
    if (rc < 0) return fail(rc, "set_rate_near");

    // Two periods of cfg.buffer_size (or 512 frames default); ALSA rounds
    // both to what the hardware supports — read the actuals back below.
    snd_pcm_uframes_t want_period =
        cfg.buffer_size != 0 ? cfg.buffer_size : 512;
    snd_pcm_uframes_t period = want_period;
    dir = 0;
    rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);
    if (rc < 0) return fail(rc, "set_period_size_near");
    snd_pcm_uframes_t buffer = period * 2;
    dir = 0;
    rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if (rc < 0) return fail(rc, "set_buffer_size_near");

    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) return fail(rc, "snd_pcm_hw_params");

    // Actuals (the *_near calls may have adjusted our request).
    snd_pcm_uframes_t actual_period = 0;
    snd_pcm_hw_params_get_period_size(hw, &actual_period, &dir);
    unsigned int actual_rate = 0;
    snd_pcm_hw_params_get_rate(hw, &actual_rate, &dir);
    snd_pcm_uframes_t actual_buffer = 0;
    snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer);
    snd_pcm_hw_params_free(hw);

    // --- Software parameters -------------------------------------------
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_malloc(&sw);
    if (sw == nullptr) {
        reason = "snd_pcm_sw_params_malloc failed";
        log::warn("audio", "alsa open failed: {}", reason);
        snd_pcm_close(pcm);
        return false;
    }
    rc = snd_pcm_sw_params_current(pcm, sw);
    if (rc < 0) {
        reason = std::string("sw_params_current: ") + snd_strerror(rc);
        log::warn("audio", "alsa open failed: {}", reason);
        snd_pcm_sw_params_free(sw);
        snd_pcm_close(pcm);
        return false;
    }
    // Start the device once the buffer is full (two written periods).
    snd_pcm_sw_params_set_start_threshold(pcm, sw, actual_buffer);
    // Let writei block until the requested frames fit (classic blocking
    // behaviour the writer thread relies on).
    snd_pcm_sw_params_set_stop_threshold(pcm, sw, actual_buffer);
    snd_pcm_sw_params_set_avail_min(pcm, sw, actual_period);
    rc = snd_pcm_sw_params(pcm, sw);
    snd_pcm_sw_params_free(sw);
    if (rc < 0) {
        reason = std::string("snd_pcm_sw_params: ") + snd_strerror(rc);
        log::warn("audio", "alsa open failed: {}", reason);
        snd_pcm_close(pcm);
        return false;
    }

    // --- Preallocate everything the writer thread needs ----------------
    auto const frames = static_cast<std::size_t>(actual_period);
    left_.assign(frames, 0.0F);
    right_.assign(frames, 0.0F);
    interleaved_.assign(frames * 2, 0.0F);
    period_frames_ = static_cast<std::uint32_t>(actual_period);
    pcm_ = pcm;

    // --- Optional capture path (live mic) -------------------------------
    // Same period geometry as playback where the device allows. "default"
    // rides the plug layer, so FLOAT_LE is near-universally negotiable;
    // S16_LE is the fallback with in-thread conversion. ANY failure here
    // downgrades to playback-only with a warning — never fatal.
    capture_ring_ = cfg.capture_ring;
    cap_pcm_ = nullptr;
    cap_s16_ = false;
    unsigned int cap_rate = approx_rate;  // logged below even on failure
    if (capture_ring_ != nullptr) {
        snd_pcm_t* cap = nullptr;
        int crc = snd_pcm_open(&cap, dev, SND_PCM_STREAM_CAPTURE, 0);
        bool cap_ok = crc >= 0;
        snd_pcm_hw_params_t* chw = nullptr;
        if (cap_ok) snd_pcm_hw_params_malloc(&chw);
        auto cap_fail = [&](char const* what, int err) {
            log::warn("audio", "alsa capture disabled ({}: {})", what,
                      snd_strerror(err));
            if (chw != nullptr) snd_pcm_hw_params_free(chw);
            if (cap != nullptr) snd_pcm_close(cap);
            cap = nullptr;
            cap_ok = false;
        };
        if (cap_ok && chw != nullptr) {
            crc = snd_pcm_hw_params_any(cap, chw);
            if (crc >= 0) {
                crc = snd_pcm_hw_params_set_access(
                    cap, chw, SND_PCM_ACCESS_RW_INTERLEAVED);
            }
            if (crc >= 0) {
                // Prefer float; a device that refuses it gets S16 + convert.
                crc = snd_pcm_hw_params_set_format(cap, chw,
                                                   SND_PCM_FORMAT_FLOAT_LE);
                if (crc < 0) {
                    crc = snd_pcm_hw_params_set_format(
                        cap, chw, SND_PCM_FORMAT_S16_LE);
                    cap_s16_ = crc >= 0;
                }
            }
            if (crc >= 0) {
                crc = snd_pcm_hw_params_set_channels(cap, chw, 2);
            }
            unsigned int cap_rate_local = cap_rate;
            int cdir = 0;
            if (crc >= 0) {
                crc = snd_pcm_hw_params_set_rate_near(cap, chw,
                                                      &cap_rate_local, &cdir);
                cap_rate = cap_rate_local;
            }
            snd_pcm_uframes_t cap_period = want_period;
            cdir = 0;
            if (crc >= 0) {
                crc = snd_pcm_hw_params_set_period_size_near(cap, chw,
                                                             &cap_period,
                                                             &cdir);
            }
            if (crc >= 0) {
                crc = snd_pcm_hw_params(cap, chw);
            }
            if (crc >= 0) {
                snd_pcm_uframes_t cap_actual = 0;
                snd_pcm_hw_params_get_period_size(chw, &cap_actual, &cdir);
                cap_frames_ = static_cast<std::uint32_t>(cap_actual);
                cap_left_.assign(cap_actual, 0.0F);
                cap_right_.assign(cap_actual, 0.0F);
                cap_raw_.assign(cap_actual * 2 * sizeof(Sample),
                                std::byte{0});
            } else {
                cap_fail("hw params", crc);
            }
        } else if (cap_ok) {
            cap_fail("hw_params_malloc", 0);
        } else {
            cap_fail("snd_pcm_open(capture)", crc);
        }
        if (cap_ok) {
            cap_pcm_ = cap;
            snd_pcm_hw_params_free(chw);
            log::info("audio",
                      "alsa capture open: device={} rate={} period={} ({})",
                      dev, cap_rate, cap_frames_,
                      cap_s16_ ? "s16le→float" : "float32");
        }
    }

    stop_.store(false, std::memory_order_relaxed);
    error_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    stats_.sample_rate.store(static_cast<std::uint32_t>(actual_rate),
                             std::memory_order_relaxed);
    stats_.buffer_size.store(static_cast<std::uint32_t>(actual_period),
                             std::memory_order_relaxed);
    log::info("audio",
              "alsa backend open: device={} rate={} period={} buffer={} "
              "frames (2 periods)",
              dev, actual_rate, actual_period, actual_buffer);
    return true;
}

void AlsaBackend::close() noexcept {
    stop();
    if (pcm_ != nullptr) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    if (cap_pcm_ != nullptr) {
        snd_pcm_close(cap_pcm_);
        cap_pcm_ = nullptr;
    }
    capture_ring_ = nullptr;
    left_.clear();
    left_.shrink_to_fit();
    right_.clear();
    right_.shrink_to_fit();
    interleaved_.clear();
    interleaved_.shrink_to_fit();
    cap_left_.clear();
    cap_left_.shrink_to_fit();
    cap_right_.clear();
    cap_right_.shrink_to_fit();
    cap_raw_.clear();
    cap_raw_.shrink_to_fit();
    period_frames_ = 512;
    cap_frames_ = 0;
}

void AlsaBackend::set_render(RenderFn fn, void* user) noexcept {
    render_user_.store(user, std::memory_order_relaxed);
    render_fn_.store(fn, std::memory_order_release);
}

void AlsaBackend::set_state_fn(StateFn fn, void* user) noexcept {
    state_user_.store(user, std::memory_order_relaxed);
    state_fn_.store(fn, std::memory_order_release);
}

bool AlsaBackend::start() {
    if (pcm_ == nullptr) {
        log::warn("audio", "alsa start called on closed backend");
        return false;
    }
    if (writer_.joinable()) writer_.join();  // paranoid: never two writers

    stop_.store(false, std::memory_order_relaxed);
    error_.store(false, std::memory_order_relaxed);
    int const rc = snd_pcm_prepare(pcm_);
    if (rc < 0) {
        log::warn("audio", "snd_pcm_prepare failed: {}", snd_strerror(rc));
        return false;
    }
    running_.store(true, std::memory_order_release);
    fire_state(StreamState::Starting);
    try {
        writer_ = std::thread([this] noexcept { writer_loop(); });
    } catch (...) {  // thread creation failure: clean false, no exception
        running_.store(false, std::memory_order_release);
        log::warn("audio", "alsa writer thread failed to start");
        return false;
    }
    if (cap_pcm_ != nullptr && capture_ring_ != nullptr) {
        if (capturer_.joinable()) capturer_.join();  // never two capturers
        snd_pcm_prepare(cap_pcm_);
        try {
            capturer_ = std::thread([this] noexcept { capture_loop(); });
        } catch (...) {
            log::warn("audio", "alsa capture thread failed to start");
        }
    }
    log::info("audio", "alsa backend running: rate={} period={}",
              stats_.sample_rate.load(std::memory_order_relaxed),
              period_frames_);
    return true;
}

void AlsaBackend::stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join();
    if (capturer_.joinable()) capturer_.join();
    // writei wakes within one period once the device drains; after the
    // joins there is nobody touching the handles, so drop is safe.
    if (pcm_ != nullptr) snd_pcm_drop(pcm_);
    if (cap_pcm_ != nullptr) snd_pcm_drop(cap_pcm_);
    running_.store(false, std::memory_order_release);
}

CallbackStats AlsaBackend::stats() const noexcept {
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

void AlsaBackend::record_xrun() noexcept {
    stats_.xruns.fetch_add(1, std::memory_order_relaxed);
}

void AlsaBackend::fire_state(StreamState s) noexcept {
    StateFn const fn = state_fn_.load(std::memory_order_acquire);
    if (fn != nullptr) {
        fn(s, state_user_.load(std::memory_order_acquire));
    }
}

void AlsaBackend::enter_error(char const* what, int err) noexcept {
    error_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    log::error("audio", "alsa writer fatal: {}: {}", what, snd_strerror(err));
    fire_state(StreamState::Error);
}

void AlsaBackend::writer_loop() noexcept {
    ghost_story::log::Logger::set_thread_name("alsa-writer");
    auto const frames = static_cast<snd_pcm_uframes_t>(period_frames_);
    for (;;) {
        if (stop_.load(std::memory_order_acquire)) return;

        // Render one period into the preallocated planar buffers, time it
        // (the timing covers the RenderFn only — comparable to the other
        // backends' callback measurement; writei blocking is expected).
        timespec t0{};
        read_monotonic(&t0);
        RenderFn const render = render_fn_.load(std::memory_order_relaxed);
        if (render != nullptr) {
            render(left_.data(), right_.data(), period_frames_,
                   render_user_.load(std::memory_order_relaxed));
        } else {
            std::memset(left_.data(), 0, left_.size() * sizeof(Sample));
            std::memset(right_.data(), 0, right_.size() * sizeof(Sample));
        }
        for (std::uint32_t i = 0; i < period_frames_; ++i) {
            interleaved_[2 * static_cast<std::size_t>(i)] = left_[i];
            interleaved_[2 * static_cast<std::size_t>(i) + 1] = right_[i];
        }
        timespec t1{};
        read_monotonic(&t1);
        std::uint32_t const us = elapsed_us(t0, t1);
        stats_.last_callback_us.store(us, std::memory_order_relaxed);
        std::uint32_t prev_max =
            stats_.max_callback_us.load(std::memory_order_relaxed);
        while (us > prev_max &&
               !stats_.max_callback_us.compare_exchange_weak(
                   prev_max, us, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }

        // Write the whole period; handle partial writes and recovery.
        snd_pcm_uframes_t written = 0;
        while (written < frames) {
            snd_pcm_sframes_t const n =
                snd_pcm_writei(pcm_, interleaved_.data() +
                                         written * 2, frames - written);
            if (n > 0) {
                written += static_cast<snd_pcm_uframes_t>(n);
                continue;
            }
            if (n == -EPIPE) {
                // Underrun: ALSA put the device into SETUP. Count it,
                // notify, re-arm; the next writei restarts the stream.
                record_xrun();
                fire_state(StreamState::Xrun);
                int const prc = snd_pcm_prepare(pcm_);
                if (prc < 0) {
                    enter_error("snd_pcm_prepare after underrun", prc);
                    return;
                }
                continue;
            }
            if (n == -ESTRPIPE) {
                // Suspend (e.g. system sleep): try to resume, bounded.
                for (int tries = 0; tries < 100; ++tries) {
                    int const rrc = snd_pcm_resume(pcm_);
                    if (rrc == 0 || rrc == -ENOSYS) break;
                    if (rrc != -EAGAIN) {
                        enter_error("snd_pcm_resume", rrc);
                        return;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{10});
                }
                int const prc = snd_pcm_prepare(pcm_);
                if (prc < 0) {
                    enter_error("snd_pcm_prepare after suspend", prc);
                    return;
                }
                continue;
            }
            if (n == 0) continue;  // no progress but no error either
            enter_error("snd_pcm_writei", static_cast<int>(n));
            return;
        }
        stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

void AlsaBackend::capture_loop() noexcept {
    ghost_story::log::Logger::set_thread_name("alsa-cap");
    auto const frames = static_cast<snd_pcm_uframes_t>(cap_frames_);
    for (;;) {
        if (stop_.load(std::memory_order_acquire)) return;
        snd_pcm_sframes_t const n = snd_pcm_readi(cap_pcm_, cap_raw_.data(),
                                                  frames);
        if (n == -EPIPE) {
            // Capture overrun (we read too slow once): re-arm and continue.
            record_xrun();
            snd_pcm_prepare(cap_pcm_);
            continue;
        }
        if (n == -ESTRPIPE) {
            for (int tries = 0; tries < 100; ++tries) {
                int const rrc = snd_pcm_resume(cap_pcm_);
                if (rrc == 0 || rrc == -ENOSYS) break;
                if (rrc != -EAGAIN) return;  // capture dies quietly
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
            snd_pcm_prepare(cap_pcm_);
            continue;
        }
        if (n <= 0) return;  // any other error: capture ends, playback lives
        auto const got = static_cast<std::uint32_t>(n);
        if (cap_s16_) {
            auto const* s16 = reinterpret_cast<std::int16_t const*>(
                cap_raw_.data());
            for (std::uint32_t i = 0; i < got; ++i) {
                cap_left_[i] = static_cast<float>(
                                   s16[2 * static_cast<std::size_t>(i)]) /
                               32768.0F;
                cap_right_[i] = static_cast<float>(
                                    s16[2 * static_cast<std::size_t>(i) + 1]) /
                                32768.0F;
            }
        } else {
            auto const* fin =
                reinterpret_cast<Sample const*>(cap_raw_.data());
            for (std::uint32_t i = 0; i < got; ++i) {
                cap_left_[i] = fin[2 * static_cast<std::size_t>(i)];
                cap_right_[i] = fin[2 * static_cast<std::size_t>(i) + 1];
            }
        }
        capture_ring_->push(cap_left_.data(), cap_right_.data(), got);
    }
}

}  // namespace ghost_story::audio
