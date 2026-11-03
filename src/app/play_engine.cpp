// addu94 — src/app/play_engine.cpp
#include "app/play_engine.hpp"

#include "log/logger.hpp"

#include <algorithm>
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
            device_connected_.store(true, std::memory_order_release);
            log::info("app", "connected {}", p.client_name);
        }
    }
    return true;
}

void PlayEngine::stop() noexcept {
    midi_.stop();
    midi_.close();
    audio_.stop();  // joins RT thread first, then synth_ is safe to touch
    synth_.all_sound_off();
}

void PlayEngine::on_midi(midi::MidiEvent const& ev) {
    // MIDI input thread: wait-free enqueue to both consumers.
    switch (ev.type) {
    case midi::Type::NoteOn:
    case midi::Type::NoteOff: {
        // Notes on channels 0/1 go to the instrument (RT queue); channel 9
        // (pads) is control-plane → app queue. Both also reach the app for
        // recording/mirror state.
        if (ev.channel != 9 && !arp_on_.load(std::memory_order_acquire)) {
            int n = ev.note() + octave_ * 12;
            if (scale_lock_.load(std::memory_order_acquire)) {
                n = music::snap_to_scale(*scale_, scale_root_, n);
            }
            VoiceCmd vc{};
            vc.note = static_cast<std::uint8_t>(n < 0 ? 0 : (n > 127 ? 127 : n));
            vc.channel = ev.channel;
            // Velocity curve (log default — mini keys under-report softs).
            vc.velocity = static_cast<std::uint8_t>(std::min(
                127, music::apply_velocity_curve(vel_curve_, ev.d2)));
            vc.on = ev.is_note_on();
            if (!synth_q_.push(vc)) {
                // Queue full under MIDI flood: drop (RT protects itself).
                log::warn("app", "synth queue full — note dropped");
            }
        }
        if (!event_q_.push(ev)) {
            log::warn("app", "event queue full — event dropped");
        }
        break;
    }
    case midi::Type::CC:
        if (ev.controller() == 64) {  // sustain pedal → RT
            if (ev.value() >= 64) {
                // M3 model: pedal-down sustains (no-op at synth level —
                // note-offs are deferred by the M6 sampler); pedal-up
                // releases everything. Documented simplification.
            } else {
                VoiceCmd vc{};
                vc.kind = VoiceCmd::Kind::PedalOff;
                if (!synth_q_.push(vc)) {
                    log::warn("app", "synth queue full — pedal dropped");
                }
            }
        }
        if (!event_q_.push(ev)) {
            log::warn("app", "event queue full — cc dropped");
        }
        break;
    default:
        if (!event_q_.push(ev)) {
            log::warn("app", "event queue full — event dropped");
        }
        break;
    }
}

void PlayEngine::on_device_event(midi::DeviceEvent const& de) {
    if (de.type == midi::DeviceEventType::PortRemoved) {
        device_connected_.store(false, std::memory_order_release);
        log::warn("app", "device port lost ({}:{}) — auto-reconnect pending",
                  de.client, de.port);
    } else if (de.type == midi::DeviceEventType::PortAdded) {
        // Reconnect any MPK-looking port within the 5 s window (spec §6.2).
        auto const ports = midi::AlsaClient::enumerate_input_ports();
        for (auto const& p : ports) {
            if (!midi::mpk_model_from_name(p.client_name).empty()) {
                if (midi_.connect_input(p.client, p.port) &&
                    midi_.connect_output(p.client, p.port)) {
                    device_connected_.store(true, std::memory_order_release);
                    log::info("app", "reconnected {}", p.client_name);
                }
            }
        }
    }
}

std::vector<mapping::Command> PlayEngine::drain_app_events() {
    // App/Qt thread: mapping + recorder + recording gate.
    std::vector<mapping::Command> all;
    midi::MidiEvent ev{};
    auto& ctrl = audio_.transport_control();
    bool const arp = arp_on_.load(std::memory_order_acquire);
    while (event_q_.pop(ev)) {
        switch (ev.type) {
        case midi::Type::NoteOn:
        case midi::Type::NoteOff:
            for (auto const& c : mapping_.consume(ev)) {
                command(c);
                all.push_back(c);
            }
            if (arp && ev.channel != 9) {
                int const n = ev.note() + octave_ * 12;
                if (n >= 0 && n <= 127) {
                    if (ev.is_note_on()) arp_.note_on(n);
                    else arp_.note_off(n);
                }
            }
            if (recording_ && ev.is_note_on() && ev.note() >= 36 &&
                ev.note() <= 51) {
                double const ticks =
                    now_ticks(audio_.transport_position(),
                              ctrl.tempo_bpm.load(std::memory_order_relaxed),
                              48000.0);
                recorder_.note_on(static_cast<std::int64_t>(ticks), ev.channel,
                                  ev.note(), ev.velocity());
            }
            break;
        case midi::Type::CC:
            for (auto const& c : mapping_.consume(ev)) {
                command(c);
                all.push_back(c);
            }
            break;
        default:
            break;
        }
    }
    if (arp) process_arp();
    return all;
}

void PlayEngine::process_arp() noexcept {
    // App thread: strike arp notes at the 1/16 grid from transport ticks.
    // Strikes ride synth_q_ so they land on the next audio buffer.
    if (arp_.empty()) {
        last_arp_grid_ = -1;
        return;
    }
    double const ticks =
        now_ticks(audio_.transport_position(),
                  audio_.transport_control().tempo_bpm.load(
                      std::memory_order_relaxed),
                  48000.0);
    std::int64_t const grid = static_cast<std::int64_t>(ticks) / 24;
    if (grid == last_arp_grid_) return;
    bool const forward = last_arp_grid_ >= 0 && grid > last_arp_grid_;
    last_arp_grid_ = grid;
    if (!forward) return;  // loop wrap or stop: wait for the next line
    auto strikes = arp_.strike(static_cast<std::uint64_t>(grid),
                               static_cast<std::uint64_t>(grid) * 2654435761u);
    arp_.advance_step();
    for (int n : strikes) {
        VoiceCmd vc{};
        vc.note = static_cast<std::uint8_t>(n < 0 ? 0 : (n > 127 ? 127 : n));
        vc.channel = 0;
        vc.velocity = 100;
        vc.on = true;
        if (!synth_q_.push(vc)) break;  // RT queue full: drop the rest
    }
}

void PlayEngine::render_into(float* left, float* right,
                             std::uint32_t frames) noexcept {
    // RT: drain synth commands, then render. Note-offs of pedal semantics:
    // VoiceCmd{note=0} + channel 0 + on==false treated as pedal-off (all
    // sound off) — sustained notes model arrives with M6 sampler.
    VoiceCmd vc{};
    while (synth_q_.pop(vc)) {
        if (vc.kind == VoiceCmd::Kind::PedalOff) {
            synth_.all_sound_off();
        } else if (vc.on) {
            synth_.note_on(vc.channel, vc.note,
                           static_cast<float>(vc.velocity) / 127.0f);
        } else {
            synth_.note_off(vc.channel, vc.note);
        }
    }
    synth_.render(left, right, frames);
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
        ctrl.tempo_bpm.store(50.0 + static_cast<double>(c.param) * 200.0 / 127.0);
        break;
    case A::ModeEdit: mapping_.set_mode(mapping::Mode::Edit); break;
    case A::ModeStep: mapping_.set_mode(mapping::Mode::Step); break;
    case A::ModePlay: mapping_.set_mode(mapping::Mode::Play); break;
    case A::ArpToggle: set_arp(!arp_on()); break;
    case A::ScaleLockToggle:
        scale_lock_.store(!scale_lock(), std::memory_order_release);
        break;
    case A::PadBankToggle: mapping_.toggle_pad_bank(); break;
    case A::OctaveUp: octave_ = std::min(octave_ + 1, 4); break;
    case A::OctaveDown: octave_ = std::max(octave_ - 1, -4); break;
    default:
        break;
    }
}

}  // namespace addu94::app
