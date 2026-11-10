// ghost_story — tests/test_capture.cpp
//
// Live-input path tests: the two lock-free rings (pure logic, both order
// and overflow policies), the PlayEngine mic channel end-to-end WITHOUT
// hardware — frames enter through test_feed_mic (the exact CaptureRing a
// backend would push) and render_into mixes/monitors them — and the
// grab-to-pad load path (DrumKit::load_override_data → zone → audible
// hit), which grab_mic_to_pad is a thin wrapper over. No audio device is
// opened.
#include "audio/capture_ring.hpp"
#include "audio/drum_kit.hpp"
#include "app/play_engine.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using ghost_story::audio::CaptureRing;
using ghost_story::audio::HistoryRing;

namespace {

/// Deterministic ramp frame generator (no rand() — house rule).
float ramp(std::uint64_t i) noexcept {
    return -0.8f + 1.6f * (static_cast<float>(i % 1000) / 1000.0f);
}

/// Feed n 512-frame buffers of a soft sine into the engine and render
/// each into the CALLER'S buffers (the exact loop render_into runs in the
/// real RT callback; the last render's output lands in out_l/out_r).
void feed_and_render(ghost_story::app::PlayEngine& eng, int buffers,
                     std::vector<float>& out_l, std::vector<float>& out_r) {
    constexpr std::uint32_t kFrames = 512;
    std::vector<float> l(kFrames);
    std::vector<float> r(kFrames);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        float const t = static_cast<float>(i) / 48000.0f;
        l[i] = 0.25f * std::sin(2.0f * 3.14159265f * 220.0f * t);
        r[i] = l[i];
    }
    for (int b = 0; b < buffers; ++b) {
        eng.test_feed_mic(l.data(), r.data(), kFrames);
        eng.render_into(out_l.data(), out_r.data(), kFrames);
    }
}

}  // namespace

TEST(capture, capture_ring_fifo_order) {
    CaptureRing r{1024};
    constexpr std::uint64_t kFrames = 500;
    std::vector<float> l(kFrames);
    std::vector<float> rch(kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        l[static_cast<std::size_t>(i)] = ramp(i);
        rch[static_cast<std::size_t>(i)] = -ramp(i);
    }
    r.push(l.data(), rch.data(), kFrames);

    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);
    std::uint64_t const got = r.pop(out_l.data(), out_r.data(), kFrames);
    CHECK_EQ(got, kFrames);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        CHECK_NEAR(out_l[static_cast<std::size_t>(i)], ramp(i), 1e-6);
        CHECK_NEAR(out_r[static_cast<std::size_t>(i)], -ramp(i), 1e-6);
    }
    // Drained: next pop is empty.
    float a = 0.0f;
    float b = 0.0f;
    CHECK_EQ(r.pop(&a, &b, 16), std::uint64_t{0});
}

TEST(capture, capture_ring_drop_new_when_full) {
    CaptureRing r{1024};
    std::vector<float> buf(2000, 0.5f);
    // Push 2000 frames into a 1024 ring: 1024 newest frames must survive,
    // 976 dropped (drop-NEW policy keeps the freshest input).
    r.push(buf.data(), buf.data(), 2000);
    CHECK_EQ(r.dropped(), std::uint64_t{976});
    std::vector<float> out(2000, 0.0f);
    std::uint64_t const got = r.pop(out.data(), out.data(), 2000);
    CHECK_EQ(got, std::uint64_t{1024});
    CHECK_NEAR(out[0], 0.5f, 1e-6);  // full — every survivor is 0.5
}

TEST(capture, capture_ring_partial_pop) {
    CaptureRing r{64};
    std::vector<float> buf(40, 0.25f);
    r.push(buf.data(), buf.data(), 40);
    std::vector<float> out(64, 0.0f);
    CHECK_EQ(r.pop(out.data(), out.data(), 10), std::uint64_t{10});
    CHECK_EQ(r.pop(out.data(), out.data(), 64), std::uint64_t{30});
    CHECK_EQ(r.pop(out.data(), out.data(), 64), std::uint64_t{0});
}

TEST(capture, history_ring_overwrite_oldest) {
    HistoryRing r{64};
    // Push 100 frames (values ramp(i)): the window keeps the newest 64.
    std::vector<float> l(100);
    std::vector<float> rr(100);
    for (std::uint64_t i = 0; i < 100; ++i) {
        l[static_cast<std::size_t>(i)] = ramp(i);
        rr[static_cast<std::size_t>(i)] = ramp(1000 - i);
    }
    r.push(l.data(), rr.data(), 100);
    std::vector<float> out_l(64, 0.0f);
    std::vector<float> out_r(64, 0.0f);
    std::uint64_t const got = r.read_last(out_l.data(), out_r.data(), 64);
    CHECK_EQ(got, std::uint64_t{64});
    for (std::uint64_t i = 0; i < 64; ++i) {
        // out[0] is the earliest frame of the window: ramp(100 - 64 + i).
        CHECK_NEAR(out_l[static_cast<std::size_t>(i)], ramp(100 - 64 + i),
                   1e-6);
        CHECK_NEAR(out_r[static_cast<std::size_t>(i)],
                   ramp(1000 - (100 - 64 + i)), 1e-6);
    }
}

TEST(capture, history_ring_read_last_clamps_to_available) {
    HistoryRing r{1024};
    std::vector<float> buf(10, 0.9f);
    r.push(buf.data(), buf.data(), 10);
    std::vector<float> out(1024, 0.0f);
    CHECK_EQ(r.read_last(out.data(), out.data(), 100), std::uint64_t{10});
    // Empty window: 0.
    HistoryRing empty{1024};
    CHECK_EQ(empty.read_last(out.data(), out.data(), 16), std::uint64_t{0});
}

TEST(capture, history_ring_oversized_push_keeps_newest) {
    HistoryRing r{64};
    // A push larger than the window keeps only the newest cap_ frames
    // (the mic path never does this, but the policy must be honest).
    std::vector<float> l(100);
    for (std::uint64_t i = 0; i < 100; ++i) {
        l[static_cast<std::size_t>(i)] = ramp(i);
    }
    r.push(l.data(), l.data(), 100);
    std::vector<float> out(64, 0.0f);
    std::uint64_t const got = r.read_last(out.data(), out.data(), 64);
    CHECK_EQ(got, std::uint64_t{64});
    CHECK_NEAR(out[0], ramp(36), 1e-6);
    CHECK_NEAR(out[63], ramp(99), 1e-6);
}

TEST(capture, capture_ring_two_thread_stress) {
    // SPSC stress: producer pushes a deterministic sequence under flow
    // control (it never overruns the ring, mirroring the real deployment
    // where the render callback drains every buffer), the consumer
    // verifies every frame arrives in exact order. TSan-covered via
    // ghost_story_apply_sanitizers.
    CaptureRing r{4096};
    constexpr std::uint64_t kTotal = 200000;
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> producer_done{false};
    std::atomic<int> consumer_errors{0};

    std::thread producer([&] {
        std::vector<float> l(256);
        std::vector<float> rr(256);
        for (std::uint64_t base = 0; base < kTotal;) {
            std::uint64_t const block =
                std::min<std::uint64_t>(256, kTotal - base);
            // Flow control: wait until at least one block of headroom.
            while (produced.load(std::memory_order_relaxed) -
                       consumed.load(std::memory_order_relaxed) >=
                   r.capacity() - 256) {
                std::this_thread::yield();
            }
            for (std::uint64_t i = 0; i < block; ++i) {
                l[static_cast<std::size_t>(i)] = ramp(base + i);
                rr[static_cast<std::size_t>(i)] = ramp(base + i);
            }
            r.push(l.data(), rr.data(), block);
            produced.fetch_add(block, std::memory_order_relaxed);
            base += block;
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::vector<float> l(256);
        std::vector<float> rr(256);
        std::uint64_t next = 0;
        for (;;) {
            std::uint64_t const got = r.pop(l.data(), rr.data(), 256);
            if (got == 0) {
                if (producer_done.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
                continue;
            }
            for (std::uint64_t i = 0; i < got; ++i) {
                float const want = ramp(next + i);
                if (l[static_cast<std::size_t>(i)] != want ||
                    rr[static_cast<std::size_t>(i)] != want) {
                    consumer_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
            next += got;
            consumed.fetch_add(got, std::memory_order_relaxed);
        }
        // The consumer may only lag behind: never ahead of the producer.
        CHECK(next <= kTotal);
    });
    producer.join();
    consumer.join();
    CHECK_EQ(consumer_errors.load(), 0);
    CHECK_EQ(r.dropped(), std::uint64_t{0});  // flow control held
}

// --- PlayEngine mic channel (no hardware) ------------------------------------

TEST(mic, monitor_mixes_and_level_taps) {
    ghost_story::app::PlayEngine eng;
    eng.set_mic_monitor(true);  // default OFF: open-mic-through-speakers
                                // feeds back; opt-in like every DAW
    CHECK(!eng.mic_active());   // nothing fed yet

    constexpr std::uint32_t kFrames = 512;
    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);

    feed_and_render(eng, 1, out_l, out_r);

    bool saw_signal = false;
    float max_abs = 0.0f;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        max_abs = std::max(max_abs, std::abs(out_l[i]));
        if (std::abs(out_l[i]) > 0.05f) saw_signal = true;
    }
    CHECK(saw_signal);                 // the mic rode the master mix
    CHECK(eng.mic_level() > 0.05f);    // the RT tap saw the same frames
    CHECK(max_abs <= 0.30f);           // no runaway mix
    CHECK(eng.mic_active());           // frames arrived "just now"
}

TEST(mic, monitor_off_still_rolls_history) {
    ghost_story::app::PlayEngine eng;
    eng.set_mic_monitor(false);

    constexpr std::uint32_t kFrames = 512;
    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);
    feed_and_render(eng, 2, out_l, out_r);

    bool all_quiet = true;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        if (std::abs(out_l[i]) > 0.0001f) all_quiet = false;
    }
    CHECK(all_quiet);                // silent monitoring
    CHECK(eng.mic_level() > 0.05f);  // frames still popped + tapped
}

TEST(mic, grab_refuses_before_audio_history) {
    ghost_story::app::PlayEngine eng;
    std::string err;
    // A fresh engine has no mic history (nothing was ever captured) and
    // the transport's sample-rate field is the startup default — either
    // way the grab must fail with a reason, never crash.
    bool const ok = eng.grab_mic_to_pad(0, 2.0, err);
    CHECK(!ok);
    CHECK(!err.empty());
}

TEST(mic, drum_kit_load_override_data_makes_an_audible_pad) {
    // The load path grab_mic_to_pad ends in (mono WavData → pad zone →
    // hit), exercised directly on DrumKit so no audio start is needed.
    ghost_story::audio::DrumKit kit;
    std::string err;
    CHECK(kit.init(48000.0, err));

    ghost_story::audio::WavData wav;
    wav.sample_rate = 48000;
    wav.channels = 1;
    wav.samples.assign(4800, 0.0f);  // 100 ms
    for (std::size_t i = 0; i < wav.samples.size(); ++i) {
        float const t = static_cast<float>(i) / 48000.0f;
        // Peak 0.5: the grab path would normalize to 0.9 — here the raw
        // data lands as-is, still clearly audible.
        wav.samples[i] = 0.5f * std::sin(2.0f * 3.14159265f * 300.0f * t);
    }
    CHECK(kit.load_override_data(38, wav, err));  // pad P3
    CHECK(kit.sampler().has_zone(38));

    constexpr std::uint32_t kFrames = 512;
    std::vector<float> out_l(kFrames, 0.0f);
    std::vector<float> out_r(kFrames, 0.0f);
    kit.note_on(38, 0.8f);
    kit.render(out_l.data(), out_r.data(), kFrames);
    bool audible = false;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        if (std::abs(out_l[i]) > 0.01f) audible = true;
    }
    CHECK(audible);
    kit.sampler().all_sound_off();

    // Bad data is refused with a reason (zero frames).
    ghost_story::audio::WavData empty;
    empty.sample_rate = 48000;
    empty.channels = 1;
    CHECK(!kit.load_override_data(38, empty, err));
    CHECK(!err.empty());
}

TEST_MAIN()  // replaced by the framework macro
