// addu94 — src/app/play_engine.hpp
//
// PlayEngine (M3/M4): the application core.
//
// Threading architecture (prime-directive compliant):
//   MIDI input thread: pushes into two lock-free SPSC queues —
//     synth_q_  : consumed by the AUDIO callback each buffer (notes, pedal)
//     event_q_  : drained by the app/Qt thread (mapping, recorder, UI)
//   Audio callback: drains synth_q_, renders synth + metronome. No locks.
//   App/Qt thread: mapping decisions, recorder, transport commands, UI.
#pragma once

#include "audio/engine.hpp"
#include "audio/synth.hpp"
#include "core/spsc_queue.hpp"
#include "mapping/mapping.hpp"
#include "midi/alsa_client.hpp"
#include "music/arp.hpp"
#include "music/scales.hpp"
#include "music/clip.hpp"
#include "music/recorder.hpp"
#include "music/velocity.hpp"

#include <atomic>
#include <string>

namespace addu94::app {

class PlayEngine : public midi::EventSink {
public:
    PlayEngine();
    ~PlayEngine() override;

    bool start(std::string& error);
    void stop() noexcept;

    // midi::EventSink (MIDI input thread): enqueue only — wait-free.
    void on_midi(midi::MidiEvent const& ev) override;
    void on_device_event(midi::DeviceEvent const& de) override;

    /// RT (audio callback): drain synth commands, render.
    void render_into(float* left, float* right, std::uint32_t frames) noexcept;

    /// App-thread: drain MIDI events through mapping/recorder. Call from a
    /// timer (Qt) or a loop; returns commands executed (for UI mirroring).
    std::vector<mapping::Command> drain_app_events();

    [[nodiscard]] audio::AudioEngine& audio() noexcept { return audio_; }
    [[nodiscard]] mapping::MappingEngine& mapping() noexcept { return mapping_; }

    // --- state queries (app thread; relaxed atomics/snapshots) ---
    [[nodiscard]] bool audio_running() const noexcept {
        return audio_.is_running();
    }
    [[nodiscard]] int selected_track() const noexcept { return selected_track_; }
    [[nodiscard]] music::Clip& current_clip() noexcept { return clip_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] int octave() const noexcept { return octave_; }
    [[nodiscard]] bool device_connected() const noexcept {
        return device_connected_.load(std::memory_order_acquire);
    }

    // --- commands (app thread) ---
    void command(mapping::Command const& c);

    // --- scale-lock (spec §2.6) ---
    [[nodiscard]] bool scale_lock() const noexcept {
        return scale_lock_.load(std::memory_order_acquire);
    }
    void set_scale_lock(bool on, std::string const& scale, int root) noexcept {
        scale_ = &music::scale_by_name(scale);
        scale_root_ = static_cast<std::uint8_t>(root % 12);
        scale_lock_.store(on, std::memory_order_release);
    }

    // --- arp state (app thread) ---
    [[nodiscard]] bool arp_on() const noexcept { return arp_on_.load(std::memory_order_acquire); }
    void set_arp(bool on) noexcept { arp_on_.store(on, std::memory_order_release); }
    [[nodiscard]] music::Arp& arp() noexcept { return arp_; }

private:
    static void render_trampoline(float* l, float* r, std::uint32_t f,
                                  void* user) noexcept;
    void process_arp() noexcept;  // app thread: strike grid crossings

    // RT synth command (trivially copyable, rides the SPSC queue).
    struct VoiceCmd {
        enum class Kind : std::uint8_t { Note, PedalOff };
        Kind kind{Kind::Note};
        std::uint8_t note{0};
        std::uint8_t channel{0};
        std::uint8_t velocity{0};
        bool on{true};
    };

    audio::AudioEngine audio_;
    midi::AlsaClient midi_;
    mapping::MappingEngine mapping_;
    audio::Synth synth_;
    music::Recorder recorder_;
    music::Clip clip_;

    static constexpr std::size_t kQueueCap = 1024;
    core::SpscQueue<VoiceCmd, kQueueCap> synth_q_;    // MIDI → audio
    core::SpscQueue<midi::MidiEvent, kQueueCap> event_q_;  // MIDI → app

    music::VelocityCurve vel_curve_{music::VelocityCurve::Log};
    int octave_{0};
    int selected_track_{0};
    float track_volume_{1.0f};
    bool recording_{false};
    std::atomic<bool> sustain_pedal_{false};
    std::atomic<bool> device_connected_{false};

    // Arp: held keys bypass synth when arp is on; arp strikes go via
    // synth_q_ on grid crossings (app thread → RT queue).
    music::Arp arp_;
    std::atomic<bool> arp_on_{false};
    std::atomic<bool> scale_lock_{false};
    music::Scale const* scale_{&music::kScales[0]};  // chromatic default
    std::uint8_t scale_root_{0};
    std::int64_t last_arp_grid_{-1};
    std::vector<int> arp_held_keys_;  // raw keys (pre-octave) while arp on
};

}  // namespace addu94::app
