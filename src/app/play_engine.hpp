// addu94 — src/app/play_engine.hpp
//
// PlayEngine (M3): the application core. Wires MIDI input → mapping →
// synth/transport/recorder → audio. Owns the audio engine and the ALSA
// client; runs one synth for the selected track (multi-track mixer lands
// with M5/M6's project model integration).
#pragma once

#include "audio/engine.hpp"
#include "audio/synth.hpp"
#include "mapping/mapping.hpp"
#include "midi/alsa_client.hpp"
#include "music/clip.hpp"
#include "music/recorder.hpp"
#include "music/velocity.hpp"

#include <string>

namespace addu94::app {

class PlayEngine : public midi::EventSink {
public:
    PlayEngine();
    ~PlayEngine() override;

    bool start(std::string& error);
    void stop() noexcept;

    // midi::EventSink (called on the MIDI input thread — RT-safe side only;
    // heavy work is bounced to queues or lock-free state).
    void on_midi(midi::MidiEvent const& ev) override;
    void on_device_event(midi::DeviceEvent const& de) override;

    /// Render node for the audio thread (master source).
    void render_into(float* left, float* right, std::uint32_t frames) noexcept;

    [[nodiscard]] audio::AudioEngine& audio() noexcept { return audio_; }
    [[nodiscard]] mapping::MappingEngine& mapping() noexcept { return mapping_; }

    // --- state queries for UI/doctor ---
    [[nodiscard]] bool audio_running() const noexcept {
        return audio_.is_running();
    }
    [[nodiscard]] int selected_track() const noexcept { return selected_track_; }
    [[nodiscard]] music::Clip& current_clip() noexcept { return clip_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] int octave() const noexcept { return octave_; }
    [[nodiscard]] int active_voices() const noexcept {
        return synth_.active_voices();
    }

    // --- commands (control thread) ---
    void command(mapping::Command const& c);

private:
    static void render_trampoline(float* l, float* r, std::uint32_t f,
                                  void* user) noexcept;
    void handle_note(midi::MidiEvent const& ev) noexcept;

    audio::AudioEngine audio_;
    midi::AlsaClient midi_;
    mapping::MappingEngine mapping_;
    audio::Synth synth_;
    music::Recorder recorder_;
    music::Clip clip_;

    music::VelocityCurve vel_curve_{music::VelocityCurve::Log};
    int octave_{0};          // keyboard octave shift (-4..4)
    int selected_track_{0};
    float track_volume_{1.0f};
    bool recording_{false};
    bool sustain_pedal_{false};
};

}  // namespace addu94::app
