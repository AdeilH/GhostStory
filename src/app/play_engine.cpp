// addu94 — src/app/play_engine.cpp
#include "app/play_engine.hpp"

#include "log/logger.hpp"

#include <cmath>

namespace addu94::app {

namespace {
double now_ticks(std::uint64_t pos_samples, double bpm, double sr) noexcept {
    double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
    return static_cast<double>(pos_samples) / (sr * spb) * 96.0;
}
}  // namespace

PlayEngine::PlayEngine() {
    clip_.length_ticks = 96 * 4 * 4;  // 4 bars default loop
    recorder_.set_clip(&clip_);
    recorder_.set_grid(music::Grid::Sixteenth);
}

PlayEngine::~PlayEngine() { stop(); }

void PlayEngine::render_trampoline(float* l, float* r, std::uint32_t f,
                                   void* user) noexcept {
    static_cast<PlayEngine*>(user)->render_into(l, r, f);
}

bool PlayEngine::start(std::string& error) {
    // Audio first (it owns the clock), then MIDI.
    audio::BackendConfig cfg;
    std::string why;
    audio_.set_master(&PlayEngine::render_trampoline, this);
    if (!audio_.start(cfg, why)) {
        error = "audio: " + why;
        return false;
    }
    if (!midi_.open("addu94", true)) {
        error = "ALSA sequencer unavailable";
        return false;
    }
    midi_.set_sink(this);
    if (!midi_.start()) {
        error = "MIDI thread failed to start";
        return false;
    }
    // Auto-connect any detected MPK.
    auto const ports = midi::AlsaClient::enumerate_input_ports();
    for (auto const& p : ports) {
        if (!midi::mpk_model_from_name(p.client_name).empty()) {
            midi_.connect_input(p.client, p.port);
            midi_.connect_output(p.client, p.port);
            log::info("app", "connected {}", p.client_name);
        }
    }
    return true;
}

void PlayEngine::stop() noexcept {
    midi_.stop();
    midi_.close();
    synth_.all_sound_off();
    audio_.stop();
}

void PlayEngine::on_midi(midi::MidiEvent const& ev) {
    // MIDI input thread — not the audio callback; small allocations here
    // are acceptable (mapping command vectors, 0-2 entries).
    auto& ctrl = audio_.transport_control();
    switch (ev.type) {
    case midi::Type::NoteOn:
    case midi::Type::NoteOff:
        if (ev.channel == 9) {
            for (auto const& c : mapping_.consume(ev)) command(c);
            if (recording_ && ev.is_note_on() && ev.note() >= 36 &&
                ev.note() <= 51) {
                double const ticks = now_ticks(
                    audio_.transport_position(),
                    ctrl.tempo_bpm.load(std::memory_order_relaxed), 48000.0);
                recorder_.note_on(static_cast<std::int64_t>(ticks), ev.channel,
                                  ev.note(), ev.velocity());
            }
        } else {
            handle_note(ev);
        }
        break;
    case midi::Type::CC:
        for (auto const& c : mapping_.consume(ev)) command(c);
        if (ev.controller() == 64) {  // sustain pedal
            sustain_pedal_ = ev.value() >= 64;
            if (!sustain_pedal_) synth_.all_sound_off();  // M3: simple
        }
        break;
    default:
        break;
    }
}

void PlayEngine::on_device_event(midi::DeviceEvent const& de) {
    if (de.type == midi::DeviceEventType::PortRemoved) {
        log::warn("app", "device port lost ({}:{}) — auto-reconnect pending",
                  de.client, de.port);
    }
}

void PlayEngine::handle_note(midi::MidiEvent const& ev) noexcept {
    if (ev.channel != 0 && ev.channel != 1) return;
    int const note = ev.note() + octave_ * 12;
    if (note < 0 || note > 127) return;
    if (ev.is_note_on()) {
        synth_.note_on(
            ev.channel, note,
            static_cast<float>(
                music::apply_velocity_curve(vel_curve_, ev.velocity())) /
                127.0f);
    } else {
        synth_.note_off(ev.channel, note);
    }
}

void PlayEngine::render_into(float* left, float* right,
                             std::uint32_t frames) noexcept {
    synth_.render(left, right, frames);
    // Track volume (M3: single track; mixer graph in M5/M6).
    float const vol = track_volume_;
    if (vol != 1.0f) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] *= vol;
            right[i] *= vol;
        }
    }
}

void PlayEngine::command(mapping::Command const& c) {
    using A = mapping::Action;
    auto& ctrl = audio_.transport_control();
    switch (c.action) {
    case A::TransportPlay:
        ctrl.state_req.store(music::TransportControl::kReqPlay);
        ctrl.metronome_on.store(true, std::memory_order_relaxed);
        break;
    case A::TransportStop:
        ctrl.state_req.store(music::TransportControl::kReqStop);
        recorder_.flush_pending();
        break;
    case A::TransportRecord:
        ctrl.state_req.store(music::TransportControl::kReqToggleRecord);
        recording_ = !recording_;
        if (recording_) recorder_.begin_take();
        else recorder_.flush_pending();
        break;
    case A::MetronomeToggle:
        ctrl.metronome_on.store(!ctrl.metronome_on.load());
        break;
    case A::Undo:
        clip_.undo_last_take();
        break;
    case A::Redo:
        // Takes are destructive on undo; redo unsupported by design (spec:
        // undo per take). Logged once via stats.
        break;
    case A::TrackVolume:
        track_volume_ = static_cast<float>(c.param) / 100.0f;
        break;
    case A::TrackSelect:
        selected_track_ = c.param;
        break;
    case A::LoopBarsSet:
        ctrl.loop_bars.store(static_cast<std::uint64_t>(c.param));
        break;
    case A::TempoNudge:
        // Knob maps 0..127 → 50..250 bpm around the center.
        ctrl.tempo_bpm.store(50.0 + static_cast<double>(c.param) * 200.0 / 127.0);
        break;
    case A::ModeEdit: mapping_.set_mode(mapping::Mode::Edit); break;
    case A::ModeStep: mapping_.set_mode(mapping::Mode::Step); break;
    case A::ModePlay: mapping_.set_mode(mapping::Mode::Play); break;
    case A::PadBankToggle: mapping_.toggle_pad_bank(); break;
    default:
        break;
    }
}

}  // namespace addu94::app
