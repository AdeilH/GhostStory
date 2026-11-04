// addu94 — src/app/play_engine.cpp
#include "app/play_engine.hpp"

#include "log/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace addu94::app {

namespace {
double now_ticks(std::uint64_t pos_samples, double bpm, double sr) noexcept {
    double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
    return static_cast<double>(pos_samples) / (sr * spb) * 96.0;
}

/// Read the knob profile (A2 — `addu94 calibrate` wrote profile.toml but
/// nothing loaded it): <project>/profile.toml when a project is attached,
/// else $XDG_CONFIG_HOME/addu94/profile.toml, else ~/.config/addu94/
/// profile.toml. App thread only (file I/O). Returns false when no file
/// exists — NOT an error, the embedded defaults then apply.
bool read_profile_toml(std::string const& project_dir, std::string& out_text,
                       std::string& out_path) {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;
    if (!project_dir.empty()) {
        candidates.push_back(project_dir + "/profile.toml");
    }
    if (char const* xdg = std::getenv("XDG_CONFIG_HOME");
        xdg != nullptr && *xdg != '\0') {
        candidates.push_back(std::string{xdg} + "/addu94/profile.toml");
    } else if (char const* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        candidates.push_back(std::string{home} + "/.config/addu94/profile.toml");
    }
    for (auto const& path : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        std::string text{std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>()};
        out_text = std::move(text);
        out_path = path;
        return true;
    }
    return false;
}
}  // namespace

PlayEngine::PlayEngine() {
    clip_.length_ticks = 96 * 4 * 4;  // 4 bars default loop
    recorder_.set_clip(&clip_);
    recorder_.set_grid(music::Grid::Sixteenth);

    for (int t = 0; t < track_count(); ++t) {
        auto const ti = static_cast<std::size_t>(t);
        track_vol_[ti].store(1.0f, std::memory_order_relaxed);
        track_pan_[ti].store(0.5f, std::memory_order_relaxed);
        track_send_[ti].store(0.0f, std::memory_order_relaxed);
    }

    // Per-track patches, chosen in code (no preset system yet). Each is a
    // deliberate DISTINCT sound — the point of the 4-track model is that
    // switching tracks is audible instantly.
    // Track 0 "Keys A": the shipped default patch, byte-identical to the
    // pre-tracks single synth (saw pair, 1200 Hz LP24) so old recordings
    // still render the same.
    synth_params_[0] = audio::SynthParams{};
    // Track 1 "Keys B": square lead — brighter cutoff, snappier envelope,
    // a touch more resonance. A/B between the key tracks must be obvious.
    audio::SynthParams keys_b{};
    keys_b.osc1 = audio::Waveform::Square;
    keys_b.osc2 = audio::Waveform::Pulse25;
    keys_b.osc_mix = 0.4f;
    keys_b.attack = 0.002f;
    keys_b.decay = 0.20f;
    keys_b.sustain = 0.55f;
    keys_b.release = 0.15f;
    keys_b.cutoff_base_hz = 2400.f;
    keys_b.resonance = 1.4f;
    synth_params_[1] = keys_b;
    // Track 3 "Bass": mono-ish sub — triangle + a sine sub an octave down,
    // dark low-keytracked filter, tight envelope. Synth has no true mono
    // mode yet; with the per-synth 64-voice pool the accidental polyphony
    // is harmless and the patch still reads as a bass.
    audio::SynthParams bass{};
    bass.osc1 = audio::Waveform::Triangle;
    bass.osc2 = audio::Waveform::Sine;
    bass.osc2_semitones = -12;
    bass.osc_mix = 0.35f;
    bass.attack = 0.003f;
    bass.decay = 0.30f;
    bass.sustain = 0.45f;
    bass.release = 0.12f;
    bass.cutoff_base_hz = 400.f;
    bass.cutoff_keytrack = 0.2f;
    bass.resonance = 0.9f;
    bass.unison_voices = 1;
    synth_params_[3] = bass;

    for (int t = 0; t < track_count(); ++t) {
        synths_[static_cast<std::size_t>(t)].set_params(
            synth_params_[static_cast<std::size_t>(t)]);
    }
}

PlayEngine::~PlayEngine() { stop(); }

void PlayEngine::render_trampoline(float* l, float* r, std::uint32_t f,
                                   void* user) noexcept {
    static_cast<PlayEngine*>(user)->render_into(l, r, f);
}

void PlayEngine::load_profile_config() {
    // attach_project() resets the flag so a late project attach still picks
    // up the project's profile.toml.
    if (profile_loaded_) return;
    profile_loaded_ = true;
    std::string text, path;
    if (!read_profile_toml(project_dir_, text, path)) {
        log::info("app",
                  "no profile.toml found — built-in knob map active (CC 1..8 "
                  "plus stock alias sets 21..28 / 70..77)");
        return;
    }
    std::string err;
    if (!mapping_.load_profile(text, err)) {
        log::warn("app", "profile {} ignored: {}", path, err);
        return;
    }
    // Log the RESOLVED knob CC map: "the knobs are doing nothing" becomes
    // diagnosable from the log alone.
    log::info("app",
              "profile {}: knob CCs 1..8 = {} {} {} {} {} {} {} {}", path,
              mapping_.knob_cc(0), mapping_.knob_cc(1), mapping_.knob_cc(2),
              mapping_.knob_cc(3), mapping_.knob_cc(4), mapping_.knob_cc(5),
              mapping_.knob_cc(6), mapping_.knob_cc(7));
}

bool PlayEngine::start(std::string& error) {
    load_profile_config();  // user/project knob map, before anything plays
    // Synthesized fallback kit (spec §4.1: playable out of the box) — all
    // four packs, so pad-pack switching never needs file I/O.
    if (!drums_.init(48000.0, error)) {
        log::warn("app", "drum kit init failed: {}", error);
    }
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

bool PlayEngine::attach_project(std::string const& dir, std::string& error) {
    project_dir_ = dir;
    if (dir.empty()) return true;  // ephemeral mode (tests)
    project::Project p;
    if (!project::load_project(dir, p, error)) {
        return false;
    }
    // Adopt loaded tempo/meter/loop into the transport control plane.
    auto& ctrl = audio_.transport_control();
    ctrl.tempo_bpm.store(p.tempo_bpm);
    ctrl.meter_num.store(static_cast<std::uint8_t>(p.meter_num));
    ctrl.meter_den.store(static_cast<std::uint8_t>(p.meter_den));
    if (!journal_.open(dir, error)) return false;
    journal_open_ = true;
    // Replay existing clip content into the working clip (track 0 clip 0).
    if (!p.tracks.empty() && !p.tracks[0].clips.empty()) {
        clip_ = p.tracks[0].clips[0];
    }
    // The project's profile.toml wins over whatever start() already loaded.
    profile_loaded_ = false;
    load_profile_config();
    log::info("app", "project loaded from {}", dir);
    return true;
}

bool PlayEngine::save_project(std::string& error) {
    if (project_dir_.empty()) return true;
    project::Project p = project::Project::make_default();
    p.tempo_bpm = audio_.transport_control().tempo_bpm.load();
    if (!p.tracks.empty()) {
        p.tracks[0].clips.push_back(clip_);
    }
    if (journal_open_) {
        if (!journal_.checkpoint(p)) {
            error = "journal checkpoint failed";
            return false;
        }
    }
    if (!journal_.save_atomic(p, project_dir_, error)) return false;
    log::info("app", "project saved to {}", project_dir_);
    return true;
}

void PlayEngine::stop() noexcept {
    midi_.stop();
    midi_.close();
    // audio_.stop() joins the RT thread first — touching the synths after
    // it is safe (same ordering argument as the old single-synth code).
    audio_.stop();
    for (auto& s : synths_) {
        s.all_sound_off();
    }
    drums_.sampler().all_sound_off();
}

void PlayEngine::push_voice(int track, int note, int velocity,
                            bool on) noexcept {
    // Wait-free: exactly one queue push per call. Called from the MIDI
    // thread (live keys) AND the app thread (arp strikes, chord slots) —
    // synth_q_ is an MPMC queue for exactly this reason (see header).
    if (track < 0 || track >= track_count()) {
        return;
    }
    VoiceCmd vc{};
    vc.velocity = static_cast<std::uint8_t>(velocity < 0 ? 0
                                                         : (velocity > 127 ? 127
                                                                           : velocity));
    vc.on = on;
    if (track == 2) {  // "Drums": any keyboard octave plays the pads
        if (!on) return;  // one-shot pads: note_off is a no-op in the Sampler
        int pad = (note - 36) % 8;  // C++ % is negative for note < 36 — fix up
        if (pad < 0) pad += 8;
        vc.track = 2;
        vc.channel = 9;
        vc.note = static_cast<std::uint8_t>(36 + pad);
    } else {
        int n = note;
        if (track == 3) n -= 12;  // Bass sounds an octave down. Live-routing
        // only: recorded clips keep the raw key, the same convention as the
        // octave shift (recording is pitch-faithful to what was played).
        vc.track = static_cast<std::uint8_t>(track);
        vc.channel = static_cast<std::uint8_t>(track_channel(track));
        vc.note = static_cast<std::uint8_t>(n < 0 ? 0 : (n > 127 ? 127 : n));
    }
    if (!synth_q_.push(vc)) {
        // Queue full under MIDI flood: drop (RT protects itself).
        log::warn("app", "synth queue full — note dropped");
    }
}

void PlayEngine::on_midi(midi::MidiEvent const& ev) {
    // MIDI input thread: wait-free enqueue to both consumers.
    switch (ev.type) {
    case midi::Type::NoteOn:
    case midi::Type::NoteOff: {
        // Hardware pads (channel 9, notes 36..43) ALWAYS hit the drum kit.
        if (ev.channel == 9 && ev.is_note_on() && ev.note() >= 36 &&
            ev.note() <= 43) {
            VoiceCmd vc{};
            vc.track = 2;
            vc.channel = 9;
            vc.note = static_cast<std::uint8_t>(ev.note());
            vc.velocity = static_cast<std::uint8_t>(
                std::min(127, music::apply_velocity_curve(vel_curve_, ev.d2)));
            vc.on = true;
            if (!synth_q_.push(vc)) {
                log::warn("app", "synth queue full — drum hit dropped");
            }
        }
        // Keys play the SELECTED track (the fix for "keys seem to stick on
        // one output for everything"): routing reads the relaxed atomic per
        // note; a track switch mid-note routes that one note to the old
        // track — harmless by design.
        if (ev.channel != 9 && !arp_on_.load(std::memory_order_acquire)) {
            int n = ev.note() + octave_ * 12;
            if (scale_lock_.load(std::memory_order_acquire)) {
                n = music::snap_to_scale(*scale_, scale_root_, n);
            }
            // Velocity curve (log default — mini keys under-report softs).
            int const vel =
                std::min(127, music::apply_velocity_curve(vel_curve_, ev.d2));
            push_voice(selected_track_.load(std::memory_order_relaxed), n,
                       vel, ev.is_note_on());
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

void PlayEngine::journal_new_events(std::size_t before) {
    if (!journal_open_) return;
    // Journal every event the recorder appended (the note pair lands at
    // note-off time — complete events only; a crash mid-note loses that
    // note, which never completed anyway).
    for (std::size_t i = before; i < clip_.events.size(); ++i) {
        auto const& e = clip_.events[i];
        nlohmann::json const j = {{"tick", e.tick},
                                  {"channel", e.channel},
                                  {"note", e.note},
                                  {"velocity", e.velocity},
                                  {"on", e.on},
                                  {"take", e.take}};
        project::JournalEntry entry;
        entry.op = project::JournalOp::ClipRecord;
        entry.i = 0;  // track 0 (M3 single-track recording)
        entry.f = 0;  // clip 0
        entry.s = j.dump();
        journal_.append(entry);
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
            if (ev.channel != 9) {
                int const n = ev.note() + octave_ * 12;
                if (n >= 0 && n <= 127) {
                    if (ev.is_note_on()) {
                        chord_held_keys_.push_back(n);
                        if (arp) arp_.note_on(n);
                    } else {
                        std::erase(chord_held_keys_, n);
                        if (arp) arp_.note_off(n);
                    }
                }
            }
            if (recording_ && !ev.is_note_on() && ev.note() >= 36 &&
                ev.note() <= 51) {
                std::size_t const before = clip_.events.size();
                recorder_.note_off(
                    static_cast<std::int64_t>(now_ticks(
                        audio_.transport_position(), ctrl.tempo_bpm.load(),
                        48000.0)),
                    ev.channel, ev.note());
                journal_new_events(before);
            }
            if (recording_ && ev.is_note_on() && ev.note() >= 36 &&
                ev.note() <= 43) {
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
    // Strikes ride synth_q_ so they land on the next audio buffer — routed
    // to the SELECTED track, consistent with live keys.
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
    int const st = selected_track_.load(std::memory_order_relaxed);
    for (int n : strikes) {
        // push_voice clamps + applies the per-track specifics (bass -12,
        // drum pad map) exactly like a live key.
        push_voice(st, n, 100, true);
    }
}

void PlayEngine::render_into(float* left, float* right,
                             std::uint32_t frames) noexcept {
    // RT: no locks, no allocation, no logging (log::warn in push_voice is
    // on the producer side). Order: drain commands -> drain knob params ->
    // mix tracks -> master gain -> meter tap.
    VoiceCmd vc{};
    while (synth_q_.pop(vc)) {
        if (vc.kind == VoiceCmd::Kind::PedalOff) {
            // Pedal-up panic: synths stop; drums are one-shots and play out.
            for (auto& s : synths_) {
                s.all_sound_off();
            }
        } else if (vc.track == 2) {
            if (vc.on) {
                drums_.note_on(vc.note,
                               static_cast<float>(vc.velocity) / 127.0f);
            }  // note-off: no-op for one-shot pads (see Sampler)
        } else {
            auto& s = synths_[static_cast<std::size_t>(vc.track)];
            if (vc.on) {
                s.note_on(vc.channel, vc.note,
                          static_cast<float>(vc.velocity) / 127.0f);
            } else {
                s.note_off(vc.channel, vc.note);
            }
        }
    }
    // Knob-driven parameter updates: a whole SynthParams per message —
    // knob turns are slow, and struct-at-once beats per-field atomics for
    // both correctness (no half-updated patch) and simplicity.
    ParamCmd pc{};
    while (param_q_.pop(pc)) {
        if (pc.track < track_count() && pc.track != 2) {
            synths_[static_cast<std::size_t>(pc.track)].set_params(pc.params);
        }
    }

    // Mix: each synth renders into the pre-allocated scratch (Synth has
    // overwrite semantics; Sampler accumulates — scratch is zeroed for it),
    // gets its track's volume x equal-power pan applied, then adds into the
    // engine's zeroed buffers. Chunked so `frames` can never overrun the
    // scratch regardless of the backend's buffer size.
    for (std::uint32_t off = 0; off < frames; off += kMaxBlock) {
        std::uint32_t const n = std::min(kMaxBlock, frames - off);
        float* L = left + off;
        float* R = right + off;
        for (int t = 0; t < track_count(); ++t) {
            auto const ti = static_cast<std::size_t>(t);
            float const vol =
                track_vol_[ti].load(std::memory_order_relaxed);
            float const pan =
                track_pan_[ti].load(std::memory_order_relaxed);
            // Equal-power pan: 0 -> full left, 0.5 -> center (-3 dB both),
            // 1 -> full right.
            float const ang = pan * 1.5707963267948966f;  // pi/2
            float const gl = vol * std::cos(ang);
            float const gr = vol * std::sin(ang);
            float* sl = scratch_l_.data();
            float* sr = scratch_r_.data();
            if (t == 2) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    sl[i] = 0.f;
                    sr[i] = 0.f;
                }
                drums_.render(sl, sr, n);
            } else {
                synths_[ti].render(sl, sr, n);
            }
            if (gl == 0.f && gr == 0.f) continue;  // muted track
            for (std::uint32_t i = 0; i < n; ++i) {
                L[i] += sl[i] * gl;
                R[i] += sr[i] * gr;
            }
        }
    }
    // Master gain (MasterVolume knob, clamped 0..1.5). Note: AudioEngine
    // renders the metronome AFTER this hook, so the click is intentionally
    // independent of the master fader (mix reference) and of track faders.
    float const mg = master_gain_.load(std::memory_order_relaxed);
    if (mg != 1.0f) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] *= mg;
            right[i] *= mg;
        }
    }
    // UI meter tap: peak |sample| over the finished buffer (RT-safe plain
    // loop, relaxed store — the reader is the Qt thread's 30 Hz tick).
    float peak = 0.0f;
    for (std::uint32_t i = 0; i < frames; ++i) {
        peak = std::max(peak, std::abs(left[i]));
        peak = std::max(peak, std::abs(right[i]));
    }
    master_level_.store(peak, std::memory_order_relaxed);
}

void PlayEngine::command(mapping::Command const& c) {
    using A = mapping::Action;
    auto& ctrl = audio_.transport_control();
    // Instrument knobs land on the SELECTED track's synth (the knob's
    // whole point). Drum track has no synth params — knobs no-op there.
    auto instrument_knob = [&](A action, int param) {
        int const t = selected_track();
        if (t < 0 || t >= track_count() || track_role(t) == TrackRole::DrumKit) {
            return;
        }
        auto& p = synth_params_[static_cast<std::size_t>(t)];
        float const x = static_cast<float>(std::clamp(param, 0, 127)) / 127.0f;
        switch (action) {
        case A::FilterCutoff:
            // Exponential 20..18000 Hz: equal octave travel per knob step
            // (a linear map would cram the useful range into the top few
            // degrees of travel).
            p.cutoff_base_hz =
                static_cast<float>(20.0 * std::pow(900.0, static_cast<double>(x)));
            break;
        case A::FilterReso:
            p.resonance = 0.5f + 9.5f * x;  // 0.5..10 (SVF Q clamp range)
            break;
        case A::EnvAttack:
            // Quadratic: fine control at the short end, where it matters.
            p.attack = 4.0f * x * x;
            break;
        case A::EnvRelease:
            p.release = 8.0f * x * x;
            break;
        case A::InstrumentMacro1:
            // One knob, two destinations (the chosen macro, documented):
            // exponential cutoff 100..8000 Hz + osc2 detune 0..25 cents —
            // a classic acid-style sweep/widen pairing. It edits the same
            // patch struct as the individual knobs, so the last knob
            // touched wins (no hidden offset state to drift).
            p.cutoff_base_hz =
                static_cast<float>(100.0 * std::pow(80.0, static_cast<double>(x)));
            p.osc2_detune_cents = 25.0f * x;
            break;
        default:
            return;
        }
        ParamCmd cmd;
        cmd.track = static_cast<std::uint8_t>(t);
        cmd.params = p;
        (void)param_q_.push(cmd);  // RT applies at the next buffer boundary
    };

    switch (c.action) {
    case A::TransportPlay:
        ctrl.state_req.store(music::TransportControl::kReqPlay);
        ctrl.metronome_on.store(true, std::memory_order_relaxed);
        break;
    case A::TransportStop: {
        ctrl.state_req.store(music::TransportControl::kReqStop);
        std::size_t const before = clip_.events.size();
        recorder_.flush_pending();
        journal_new_events(before);
        std::string err;
        if (!save_project(err)) {
            log::error("app", "project save failed: {}", err);
        }
        break;
    }
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
        // 0..127 knob -> 0..1 per-track volume. Applied RT as the track's
        // render gain (see render_into) — set-before-render, no locks.
        track_vol_[static_cast<std::size_t>(selected_track())].store(
            static_cast<float>(std::clamp(c.param, 0, 127)) / 127.0f, std::memory_order_relaxed);
        break;
    case A::TrackPan:
        track_pan_[static_cast<std::size_t>(selected_track())].store(
            static_cast<float>(std::clamp(c.param, 0, 127)) / 127.0f, std::memory_order_relaxed);
        break;
    case A::TrackSend:
        // Stored for the FX-send milestone — src/audio/fx.hpp's FxChain is
        // an INSERT chain with no shared send bus, so wiring a real send
        // would mean building one; nothing is faked meanwhile.
        track_send_[static_cast<std::size_t>(selected_track())].store(
            static_cast<float>(std::clamp(c.param, 0, 127)) / 127.0f, std::memory_order_relaxed);
        break;
    case A::MasterVolume:
        // 0..127 -> 0..1.5 (unity sits at ~85 so the knob has headroom).
        master_gain_.store(
            std::clamp(1.5f * static_cast<float>(std::clamp(c.param, 0, 127)) /
                           127.0f,
                       0.0f, 1.5f),
            std::memory_order_relaxed);
        break;
    case A::ClickLevel:
        // 0..127 -> click gain 0..1.0 (default 0.35 sits at ~45).
        ctrl.metronome_gain.store(
            static_cast<float>(std::clamp(c.param, 0, 127)) / 127.0f, std::memory_order_relaxed);
        break;
    case A::QuantizeGridSet:
        // Four zones across the knob sweep: off / 1/8 / 1/16 / 1/32
        // (quartile thresholds — a committed knob position is a committed
        // grid; no hysteresis needed on a set-value knob).
        recorder_.set_grid(c.param < 32  ? music::Grid::Off
                           : c.param < 64 ? music::Grid::Eighth
                           : c.param < 96 ? music::Grid::Sixteenth
                                          : music::Grid::ThirtySecond);
        break;
    case A::ScrubCoarse:
        // Stored until Transport grows a position-set control (see
        // scrub_position) — coarse knob = whole bars.
        scrub_coarse_ = std::clamp(c.param, 0, 127);
        break;
    case A::ScrubFine:
        scrub_fine_ = std::clamp(c.param, 0, 127);  // 1/128 bar steps
        break;
    case A::FilterCutoff:
    case A::FilterReso:
    case A::EnvAttack:
    case A::EnvRelease:
    case A::InstrumentMacro1:
        instrument_knob(c.action, c.param);
        break;
    case A::TrackSelect:
        select_track(c.param);
        break;
    case A::DrumPad:
        // Sound came via the RT queue; recorder records in drain. Nothing
        // else to do — the pad's response is its distinct drum hit.
        break;
    case A::ChordSlot: {
        int const slot = c.param;
        if (slot < 0 || slot >= kChordSlots) break;
        if (c.delta == 0) {
            chord_press_[static_cast<std::size_t>(slot)] =
                std::chrono::steady_clock::now();
            // Trigger stored chord (note-ons), unless empty. The chord
            // sounds on the SELECTED track at press time; the matching
            // offs go to the same track even if selection changes.
            auto const& chord = chord_slots_[static_cast<std::size_t>(slot)];
            if (!chord.empty() && chord_sounding_.empty()) {
                int const st = selected_track();
                for (int n : chord) {
                    push_voice(st, n, 100, true);
                }
                chord_sounding_ = chord;
                chord_sounding_track_ = st;
            }
        } else {
            auto const held_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                           chord_press_[static_cast<
                                               std::size_t>(slot)])
                .count();
            if (held_ms >= 400 && !chord_held_keys_.empty()) {
                // Capture: hold slot + play keys -> store the chord.
                chord_slots_[static_cast<std::size_t>(slot)] =
                    chord_held_keys_;
                log::info("app", "chord slot {} captured ({} notes)", slot,
                          chord_held_keys_.size());
            }
            // Note-offs for a sounding chord (same track as the ons).
            if (!chord_sounding_.empty()) {
                int const st = chord_sounding_track_ >= 0
                                   ? chord_sounding_track_
                                   : selected_track();
                for (int n : chord_sounding_) {
                    push_voice(st, n, 0, false);
                }
                chord_sounding_.clear();
                chord_sounding_track_ = -1;
            }
        }
        break;
    }
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

// --- track API ------------------------------------------------------------

char const* PlayEngine::track_name(int idx) const noexcept {
    static char const* const kNames[track_count()] = {"Keys A", "Keys B",
                                                      "Drums", "Bass"};
    return idx >= 0 && idx < track_count()
               ? kNames[static_cast<std::size_t>(idx)]
               : "";
}

TrackRole PlayEngine::track_role(int idx) const noexcept {
    static constexpr TrackRole kRoles[track_count()] = {
        TrackRole::SynthA, TrackRole::SynthB, TrackRole::DrumKit,
        TrackRole::Bass};
    return idx >= 0 && idx < track_count()
               ? kRoles[static_cast<std::size_t>(idx)]
               : TrackRole::SynthA;
}

void PlayEngine::select_track(int idx) noexcept {
    if (idx < 0) idx = 0;
    if (idx >= track_count()) idx = track_count() - 1;
    // Relaxed: per-note routing reads this; the only race is one note
    // landing on the previous track at the instant of switching — harmless.
    selected_track_.store(idx, std::memory_order_relaxed);
}

// --- pad packs ------------------------------------------------------------

std::vector<PlayEngine::PadPackInfo> const& PlayEngine::pad_packs()
    const noexcept {
    // Built once from DrumKit's static table (names are stable string
    // literals); the vector never mutates afterwards.
    static std::vector<PadPackInfo> const kPacks = [] {
        std::vector<PadPackInfo> v;
        v.reserve(static_cast<std::size_t>(audio::DrumKit::pack_count()));
        for (int i = 0; i < audio::DrumKit::pack_count(); ++i) {
            v.push_back(PadPackInfo{i, audio::DrumKit::pack_name(i)});
        }
        return v;
    }();
    return kPacks;
}

bool PlayEngine::set_pad_pack(int index, std::string& error) {
    // DrumKit::load_pack validates the index and the kit state; the switch
    // itself is a single atomic store on the RT side (see DrumKit header).
    return drums_.load_pack(index, error);
}

void PlayEngine::cycle_pad_pack(std::string& error) {
    int const next = (current_pad_pack() + 1) % audio::DrumKit::pack_count();
    set_pad_pack(next, error);
}

}  // namespace addu94::app
