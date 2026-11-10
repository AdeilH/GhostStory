// ghost_story — src/app/play_engine.cpp
#include "app/play_engine.hpp"

#include "log/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ghost_story::app {

namespace {
double now_ticks(std::uint64_t pos_samples, double bpm, double sr) noexcept {
    double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
    return static_cast<double>(pos_samples) / (sr * spb) * 96.0;
}

/// Steady-clock milliseconds (vDSO read — the same measurement the audio
/// backends use for callback timing, so an RT stamp per buffer is fine).
std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// (Record-arm channel mapping moved into PlayEngine::channel_track — it
// needs the dynamic slot table now.)

/// Names the old fixed-4 engine used — legacy project files restore them.
constexpr char const* kLegacyNames[4] = {"Keys A", "Keys B", "Drums",
                                         "Bass"};

/// Read the knob profile (A2 — `ghost_story calibrate` wrote profile.toml but
/// nothing loaded it): <project>/profile.toml when a project is attached,
/// else $XDG_CONFIG_HOME/ghost_story/profile.toml, else ~/.config/ghost_story/
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
        candidates.push_back(std::string{xdg} + "/ghost_story/profile.toml");
    } else if (char const* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        candidates.push_back(std::string{home} + "/.config/ghost_story/profile.toml");
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

// ---------------------------------------------------------------- presets
// Curated {synth patch + FX chain} pairs for the keys tracks. Every preset
// exercises different blocks of the (new) chain — chorus/flanger/tremolo/
// comp/crush/drive/reverb — so applying them audibly demos the FX. Index
// order is stable (FxPanel chips + MANUAL.md table reference it).
struct PlayEnginePreset {
    char const* name;
    audio::SynthParams synth;
    audio::FxParams fx;
};

audio::SynthParams preset_synth(audio::Waveform w1, audio::Waveform w2,
                                float mix, float a, float d, float s,
                                float r, float cut, float reso) noexcept {
    audio::SynthParams p{};
    p.osc1 = w1;
    p.osc2 = w2;
    p.osc_mix = mix;
    p.attack = a;
    p.decay = d;
    p.sustain = s;
    p.release = r;
    p.cutoff_base_hz = cut;
    p.resonance = reso;
    return p;
}

PlayEnginePreset const kPresetTable[] = {
    {"Default Saw",
     preset_synth(audio::Waveform::Saw, audio::Waveform::Saw, 0.5f, 0.005f,
                  0.15f, 0.7f, 0.25f, 1200.f, 0.7f),
     {}},  // the shipped Keys A sound: everything bypass
    {"Acid Bass",
     [] {
         auto p = preset_synth(audio::Waveform::Saw, audio::Waveform::Saw,
                               0.35f, 0.002f, 0.18f, 0.35f, 0.10f, 300.f,
                               6.0f);
         p.mono_legato = true;
         p.glide_seconds = 0.08f;
         p.cutoff_keytrack = 0.5f;
         p.velocity_filter = 0.4f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.drive = 2.0f;  // soft tanh growl
         f.delay_on = true;
         f.delay_beats = 0.125f;
         f.delay_mix = 0.15f;
         return f;
     }()},
    {"Warm Pad",
     [] {
         auto p = preset_synth(audio::Waveform::Saw, audio::Waveform::Saw,
                               0.5f, 0.80f, 0.60f, 0.80f, 1.20f, 900.f,
                               0.7f);
         p.osc2_detune_cents = 12.f;
         p.unison_voices = 3;
         p.unison_spread_cents = 18.f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.chorus_on = true;
         f.chorus_mix = 0.35f;
         f.reverb_on = true;
         f.reverb_mix = 0.35f;
         f.reverb_decay = 0.75f;
         return f;
     }()},
    {"Pluck Keys",
     [] {
         auto p = preset_synth(audio::Waveform::Triangle,
                               audio::Waveform::Square, 0.30f, 0.002f,
                               0.18f, 0.0f, 0.30f, 2500.f, 1.2f);
         p.velocity_filter = 0.5f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.delay_on = true;
         f.delay_beats = 0.125f;
         f.delay_feedback = 0.35f;
         f.delay_mix = 0.20f;
         return f;
     }()},
    {"Square Lead",
     [] {
         auto p = preset_synth(audio::Waveform::Square,
                               audio::Waveform::Pulse25, 0.40f, 0.002f,
                               0.20f, 0.55f, 0.15f, 3000.f, 1.4f);
         p.mono_legato = true;
         p.glide_seconds = 0.03f;
         p.lfo_target = audio::LfoTarget::Pitch;  // vibrato
         p.lfo_rate_hz = 5.5f;
         p.lfo_depth = 0.15f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.delay_on = true;
         f.delay_beats = 0.25f;
         f.delay_mix = 0.25f;
         return f;
     }()},
    {"Sub Bass",
     [] {
         auto p = preset_synth(audio::Waveform::Sine,
                               audio::Waveform::Triangle, 0.35f, 0.003f,
                               0.30f, 0.45f, 0.12f, 300.f, 0.9f);
         p.osc2_semitones = -12;
         p.mono_legato = true;
         p.glide_seconds = 0.06f;
         p.unison_voices = 1;
         p.velocity_filter = 0.3f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.comp_on = true;
         f.comp_threshold = 0.30f;
         f.comp_ratio = 3.0f;
         f.comp_makeup = 1.2f;
         return f;
     }()},
    {"Dreamy Keys",
     [] {
         auto p = preset_synth(audio::Waveform::Triangle,
                               audio::Waveform::Sine, 0.5f, 0.40f, 0.50f,
                               0.70f, 0.90f, 1400.f, 0.8f);
         p.unison_voices = 2;
         p.unison_spread_cents = 12.f;
         p.lfo2_target = audio::LfoTarget::Amp;
         p.lfo2_rate_hz = 0.4f;
         p.lfo2_depth = 0.15f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.chorus_on = true;
         f.chorus_mix = 0.30f;
         f.flanger_on = true;
         f.flanger_mix = 0.20f;
         f.reverb_on = true;
         f.reverb_mix = 0.40f;
         f.reverb_decay = 0.80f;
         return f;
     }()},
    {"Stabs",
     preset_synth(audio::Waveform::Saw, audio::Waveform::Saw, 0.4f, 0.002f,
                  0.12f, 0.30f, 0.15f, 1200.f, 1.0f),
     [] {
         audio::FxParams f{};
         f.comp_on = true;
         f.comp_threshold = 0.25f;
         f.comp_ratio = 2.5f;
         f.delay_on = true;
         f.delay_beats = 0.125f;
         f.delay_feedback = 0.45f;
         f.delay_mix = 0.30f;
         return f;
     }()},
    {"Lo-Fi Keys",
     [] {
         auto p = preset_synth(audio::Waveform::Triangle,
                               audio::Waveform::Sine, 0.5f, 0.01f, 0.25f,
                               0.60f, 0.30f, 1600.f, 0.8f);
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.eq_low_db = 4.f;
         f.eq_high_db = -6.f;
         f.crush_on = true;
         f.crush_bits = 8.f;
         f.crush_downsample = 2.f;
         f.drive = 1.8f;
         f.tremolo_on = true;
         f.tremolo_rate_hz = 2.2f;
         f.tremolo_depth = 0.25f;
         return f;
     }()},
    {"Fly Lead",
     [] {
         auto p = preset_synth(audio::Waveform::Saw, audio::Waveform::Saw,
                               0.5f, 0.003f, 0.25f, 0.60f, 0.20f, 4000.f,
                               2.0f);
         p.osc2_semitones = 12;
         p.unison_voices = 3;
         p.unison_spread_cents = 25.f;
         p.mono_legato = true;
         p.glide_seconds = 0.05f;
         return p;
     }(),
     [] {
         audio::FxParams f{};
         f.delay_on = true;
         f.delay_beats = 0.125f;
         f.delay_feedback = 0.50f;
         f.delay_mix = 0.35f;
         f.reverb_on = true;
         f.reverb_mix = 0.20f;
         return f;
     }()},
};

constexpr int kPresetCount =
    static_cast<int>(sizeof(kPresetTable) / sizeof(kPresetTable[0]));
}  // namespace

PlayEngine::PlayEngine() {
    // The pool starts EMPTY (user: "I will add them manually as needed") —
    // every slot's mixer state and clip are initialized so add_track is a
    // pure activation.
    for (std::size_t t = 0; t < clips_.size(); ++t) {
        clips_[t].length_ticks = 96 * 4 * 4;  // 4 bars default loop
        track_vol_[t].store(1.0f, std::memory_order_relaxed);
        track_pan_[t].store(0.5f, std::memory_order_relaxed);
        track_send_[t].store(0.0f, std::memory_order_relaxed);
        track_preset_[t].store(-1, std::memory_order_relaxed);
        synth_params_[t] = audio::SynthParams{};
        synths_[t].set_params(synth_params_[t]);
    }
    // Legacy patches for projects saved by the old fixed-4 engine (kept
    // byte-identical so old recordings still render the same): Keys B's
    // square lead and Bass's mono sub (with its live -12 as slot
    // transpose). attach_project wires them by slot for legacy files.
    legacy_keys_b_.osc1 = audio::Waveform::Square;
    legacy_keys_b_.osc2 = audio::Waveform::Pulse25;
    legacy_keys_b_.osc_mix = 0.4f;
    legacy_keys_b_.attack = 0.002f;
    legacy_keys_b_.decay = 0.20f;
    legacy_keys_b_.sustain = 0.55f;
    legacy_keys_b_.release = 0.15f;
    legacy_keys_b_.cutoff_base_hz = 2400.f;
    legacy_keys_b_.resonance = 1.4f;
    legacy_bass_.osc1 = audio::Waveform::Triangle;
    legacy_bass_.osc2 = audio::Waveform::Sine;
    legacy_bass_.osc2_semitones = -12;
    legacy_bass_.osc_mix = 0.35f;
    legacy_bass_.attack = 0.003f;
    legacy_bass_.decay = 0.30f;
    legacy_bass_.sustain = 0.45f;
    legacy_bass_.release = 0.12f;
    legacy_bass_.cutoff_base_hz = 400.f;
    legacy_bass_.cutoff_keytrack = 0.2f;
    legacy_bass_.resonance = 0.9f;
    legacy_bass_.unison_voices = 1;
    legacy_bass_.mono_legato = true;
    legacy_bass_.glide_seconds = 0.04f;

    recorder_.set_grid(music::Grid::Sixteenth);
    // Master hook from the CONSTRUCTOR, not start(): the render callback
    // must always run the engine mix. (It used to be installed in start()
    // only — any test/test-harness driving audio_.render() without start()
    // silently rendered nothing but the metronome, which is exactly why
    // the first "audible take" tests were green for the wrong reason.)
    audio_.set_master(&PlayEngine::render_trampoline, this);
    republish_clips();  // empty snapshots; keeps published_* bookkeeping warm
    // Rolling mic-grab window (control-thread allocation, ~4 MB). Done in
    // the constructor, not start(): grabs must work before audio ever
    // started (and tests drive the engine directly).
    mic_history_ = std::make_unique<audio::HistoryRing>(1u << 19);
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
    // Audio first (it owns the clock), then MIDI.
    audio::BackendConfig cfg;
    // Synthesized fallback kit (spec §4.1: playable out of the box) — all
    // packs, so pad-pack switching never needs file I/O. The kit runs at
    // the BACKEND's rate (the old hardcoded 48000 mis-tuned it elsewhere).
    if (!drums_.init(static_cast<double>(cfg.sample_rate), error)) {
        log::warn("app", "drum kit init failed: {}", error);
    }
    last_kit_error_ = error;
    // Live input: the capture ring (history allocated in the ctor) hands
    // the backend a place to push frames that the RT callback pops.
    // Backends without a capture path ignore the pointer and the mic
    // panel shows "no input".
    cfg.capture_ring = &mic_ring_;
    // FX chains + synths: one-time setup for the WHOLE pool BEFORE the
    // audio thread exists (FxChain::set_sample_rate is the only allocating
    // call — after this, adding a track mid-session is pure control-thread
    // bookkeeping: activate a slot, push params, republish). Same rate the
    // backends request below; the synths previously never learned the real
    // rate at all (they silently ran on their 48000 default).
    for (std::size_t t = 0; t < fx_chains_.size(); ++t) {
        fx_chains_[t].set_sample_rate(static_cast<double>(cfg.sample_rate));
        fx_chains_[t].set_tempo(
            audio_.transport_control().tempo_bpm.load(std::memory_order_relaxed));
        synths_[t].set_sample_rate(static_cast<double>(cfg.sample_rate));
    }
    fx_tempo_ = audio_.transport_control().tempo_bpm.load(
        std::memory_order_relaxed);
    std::string why;
    if (!audio_.start(cfg, why)) {
        error = "audio: " + why;
        return false;
    }
    if (!midi_.open("ghost_story", true)) {
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
    // Adopt the saved tracks: kind from `instrument`, name + mixer state +
    // clip (with placement). Legacy heuristic: files saved by the old
    // fixed-4 engine wrote four default "Track N" rows ALL instrumented
    // "synth" — restore their index layout (2 = the shared kit, 3 = Bass
    // with its live -12 transpose) and their byte-identical patches so old
    // recordings still sound the same.
    bool const legacy =
        p.tracks.size() == 4 &&
        std::all_of(p.tracks.begin(), p.tracks.end(),
                    [](project::Track const& t) {
                        return t.instrument == "synth";
                    });
    int const count = static_cast<int>(
        std::min<std::size_t>(p.tracks.size(), clips_.size()));
    for (int t = 0; t < count; ++t) {
        auto const ti = static_cast<std::size_t>(t);
        auto const& pt = p.tracks[ti];
        bool const drums = legacy ? (t == 2) : (pt.instrument == "drumkit");
        slots_[ti].kind.store(
            static_cast<std::uint8_t>(drums ? TrackKind::Drums
                                            : TrackKind::Synth),
            std::memory_order_relaxed);
        slots_[ti].name = legacy ? kLegacyNames[ti] : pt.name;
        slots_[ti].transpose.store(
            legacy && t == 3 ? -12 : 0, std::memory_order_relaxed);
        track_vol_[ti].store(std::clamp(pt.volume, 0.0f, 1.5f),
                             std::memory_order_relaxed);
        track_pan_[ti].store(std::clamp((pt.pan + 1.0f) * 0.5f, 0.0f, 1.0f),
                             std::memory_order_relaxed);
        track_mute_[ti].store(pt.mute, std::memory_order_relaxed);
        track_preset_[ti].store(-1, std::memory_order_relaxed);
        if (!drums) {
            synth_params_[ti] = legacy ? (t == 1   ? legacy_keys_b_
                                          : t == 3 ? legacy_bass_
                                                   : audio::SynthParams{})
                                       : audio::SynthParams{};
            synths_[ti].set_params(synth_params_[ti]);
            if (!param_q_.push(
                    {static_cast<std::uint8_t>(t), synth_params_[ti]})) {
                log::warn("app", "param queue full — patch lands next buffer");
            }
        }
        if (!pt.clips.empty()) {
            clips_[ti] = pt.clips[0];
        }
    }
    active_.store(count, std::memory_order_release);
    select_track(0);
    republish_clips();
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
    // Real per-track persistence (the old save wrote 4 default "Track N"
    // rows with clips only — names/kinds/mixer state were lost). Pan: the
    // document stores -1..1, the engine 0..1.
    p.tracks.clear();
    for (int t = 0; t < track_count(); ++t) {
        auto const ti = static_cast<std::size_t>(t);
        project::Track tr;
        tr.name = slots_[ti].name;
        tr.instrument = track_kind(t) == TrackKind::Drums ? "drumkit"
                                                          : "synth";
        tr.volume = track_vol_[ti].load(std::memory_order_relaxed);
        tr.pan = track_pan_[ti].load(std::memory_order_relaxed) * 2.0f - 1.0f;
        tr.mute = track_mute_[ti].load(std::memory_order_relaxed);
        tr.clips.push_back(clips_[ti]);
        p.tracks.push_back(std::move(tr));
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
    if (track_kind(track) == TrackKind::Drums) {
        // Drums: any keyboard octave plays the pads (shared kit).
        if (!on) return;  // one-shot pads: note_off is a no-op in the Sampler
        int pad = (note - 36) % 8;  // C++ % is negative for note < 36 — fix up
        if (pad < 0) pad += 8;
        vc.track = static_cast<std::uint8_t>(track);
        vc.channel = 9;
        vc.note = static_cast<std::uint8_t>(36 + pad);
    } else {
        int n = note + slots_[static_cast<std::size_t>(track)].transpose.load(
                        std::memory_order_relaxed);
        // Slot transpose (legacy Bass -12) is a live-routing transform:
        // recorded clips keep the raw key, the same convention as the
        // octave shift (recording is pitch-faithful to what was played).
        vc.track = static_cast<std::uint8_t>(track);
        vc.channel = static_cast<std::uint8_t>(track);  // slot = voice identity
        vc.note = static_cast<std::uint8_t>(n < 0 ? 0 : (n > 127 ? 127 : n));
    }
    if (!synth_q_.push(vc)) {
        // Queue full under MIDI flood: drop (RT protects itself).
        log::warn("app", "synth queue full — note dropped");
    }
}

void PlayEngine::apply_voice(int track, int channel, int note, int velocity,
                             bool on) noexcept {
    // RT thread only: the synth_q_ drain and the clip players' fires both
    // land here. Drums are one-shots (note-off is a no-op in the Sampler).
    if (track >= 0 && track < track_count() &&
        track_kind(track) == TrackKind::Drums) {
        if (on) {
            drums_.note_on(note, static_cast<float>(velocity) / 127.0f);
        }
    } else if (track >= 0 && track < track_count()) {
        auto& s = synths_[static_cast<std::size_t>(track)];
        if (on) {
            s.note_on(channel, note, static_cast<float>(velocity) / 127.0f);
        } else {
            s.note_off(channel, note);
        }
    }
}

void PlayEngine::test_init_audio() noexcept {
    // The start()-without-backends subset at the default 48 kHz — enough
    // for tests to hear real takes without opening audio devices.
    audio::BackendConfig cfg;
    if (!drums_.init(static_cast<double>(cfg.sample_rate), last_kit_error_)) {
        log::warn("app", "test kit init failed: {}", last_kit_error_);
    }
    for (std::size_t t = 0; t < fx_chains_.size(); ++t) {
        fx_chains_[t].set_sample_rate(static_cast<double>(cfg.sample_rate));
        fx_chains_[t].set_tempo(
            audio_.transport_control().tempo_bpm.load(std::memory_order_relaxed));
        synths_[t].set_sample_rate(static_cast<double>(cfg.sample_rate));
    }
}

float PlayEngine::test_render_audio(std::uint32_t frames,
                                    std::uint32_t count) noexcept {
    // Test-only driver (selftest-record's playback assertion): run the REAL
    // callback synchronously on the calling thread — transport requests
    // apply, position advances, clip scheduling runs, the full mix renders.
    // Heap buffers are fine here; the test thread is not RT.
    std::vector<float> l(frames, 0.0f);
    std::vector<float> r(frames, 0.0f);
    float peak = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        audio_.render(l.data(), r.data(), frames);
        for (std::uint32_t j = 0; j < frames; ++j) {
            peak = std::max(peak, std::abs(l[j]));
            peak = std::max(peak, std::abs(r[j]));
        }
    }
    return peak;
}

void PlayEngine::on_midi(midi::MidiEvent const& ev) {
    // MIDI input thread: wait-free enqueue to both consumers.
    switch (ev.type) {
    case midi::Type::NoteOn:
    case midi::Type::NoteOff: {
        // Pad drums are a PLAY-mode thing (pads semantics v2): all 16 pads
        // fire the kit — 36..43 natural, 44..51 the same sounds +12 (the
        // Sampler's pad mapping does the shift). In EDIT the pads select
        // tracks / undo / chords and in KIT they pick packs — mode-gated
        // so a chord-capture or pack-select press never fires a stray hit
        // (and drumming can never reach Undo: undo/redo live in EDIT and
        // Ctrl+Z only). The gate reads the relaxed mode_rt_ atomic, written
        // only by set_mode; a one-note race at a mode switch is harmless
        // (same contract as selected_track_). Keys (channel != 9) are NOT
        // gated — they always play the selected track.
        int const kit_slot = drums_slot();
        if (ev.channel == 9 && ev.is_note_on() && ev.note() >= 36 &&
            ev.note() <= 51 && kit_slot >= 0 &&
            mode_rt_.load(std::memory_order_relaxed) == 0) {
            VoiceCmd vc{};
            vc.track = static_cast<std::uint8_t>(kit_slot);
            vc.channel = 9;
            vc.note = static_cast<std::uint8_t>(ev.note());
            vc.velocity = static_cast<std::uint8_t>(std::min(
                127,
                music::apply_velocity_curve(
                    vel_curve_.load(std::memory_order_relaxed), ev.d2)));
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
            // Play-once guard (user report: "played multiple times on
            // pressing"): drop re-fires of an already-held key (hardware
            // arpeggiator / double trigger). Note-offs always pass so the
            // voice releases. is_note_on() is false for the velocity-0
            // NoteOn note-off form, which correctly clears the mark.
            if (retrigger_.allow(ev.channel, ev.d1, ev.is_note_on(),
                                 ev.stamp_ns)) {
                int n = ev.note() + octave_ * 12;
                if (scale_lock_.load(std::memory_order_acquire)) {
                    n = music::snap_to_scale(*scale_, scale_root_, n);
                }
                // Velocity curve (log default — mini keys under-report
                // softs).
                int const vel = std::min(
                    127, music::apply_velocity_curve(
                             vel_curve_.load(std::memory_order_relaxed),
                             ev.d2));
                push_voice(selected_track_.load(std::memory_order_relaxed), n,
                           vel, ev.is_note_on());
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

void PlayEngine::journal_new_events(int track, std::size_t before) {
    if (!journal_open_) return;
    // Journal every event the recorder appended (the note pair lands at
    // note-off time — complete events only; a crash mid-note loses that
    // note, which never completed anyway).
    auto& clip = clips_[static_cast<std::size_t>(track)];
    for (std::size_t i = before; i < clip.events.size(); ++i) {
        auto const& e = clip.events[i];
        nlohmann::json const j = {{"tick", e.tick},
                                  {"channel", e.channel},
                                  {"note", e.note},
                                  {"velocity", e.velocity},
                                  {"on", e.on},
                                  {"take", e.take}};
        project::JournalEntry entry;
        entry.op = project::JournalOp::ClipRecord;
        entry.i = track;  // the recording track (loop machine: per-track)
        entry.f = 0;      // clip 0 of that track
        entry.s = j.dump();
        journal_.append(entry);
    }
}

void PlayEngine::republish_clips() noexcept {
    // Push changed clips (or a moved tempo/rate) into the RT players. All
    // clip mutations insert or erase, so (size, length) catches every
    // change without a revision counter.
    double const bpm = audio_.transport_control().tempo_bpm.load(
        std::memory_order_relaxed);
    double const sr = app_sample_rate();
    for (int t = 0; t < track_count(); ++t) {
        auto const ti = static_cast<std::size_t>(t);
        auto const shape =
            std::tuple<std::size_t, std::int64_t, std::int64_t>{
                clips_[ti].events.size(), clips_[ti].length_ticks,
                clips_[ti].start_tick};
        if (published_bpm_ == bpm && published_sr_ == sr &&
            published_shape_[ti] == shape) {
            continue;
        }
        clip_players_[ti].publish(clips_[ti], bpm, sr);
        published_shape_[ti] = shape;
    }
    published_bpm_ = bpm;
    published_sr_ = sr;
}

double PlayEngine::app_sample_rate() noexcept {
    double const sr = audio_.stats().sample_rate;
    return (sr >= 8000.0 && sr <= 192000.0) ? sr : 48000.0;
}

int PlayEngine::clip_bars(int track) noexcept {
    if (track < 0 || track >= track_count()) return 0;
    double const beats_per_bar =
        static_cast<double>(
            audio_.transport_control().meter_num.load(
                std::memory_order_relaxed));
    double const bars =
        static_cast<double>(clip(track).length_ticks) /
        (static_cast<double>(music::kPPQ) * beats_per_bar);
    return std::max(1, static_cast<int>(
                           std::llround(bars > 0.0 ? bars : 1.0)));
}

int PlayEngine::clip_start_bar(int track) noexcept {
    if (track < 0 || track >= track_count()) return 0;
    double const beats_per_bar =
        static_cast<double>(
            audio_.transport_control().meter_num.load(
                std::memory_order_relaxed));
    double const bars = static_cast<double>(clip(track).start_tick) /
                        (static_cast<double>(music::kPPQ) * beats_per_bar);
    return std::max(0, static_cast<int>(std::llround(bars)));
}

void PlayEngine::set_playhead_bar(int bar) {
    // Click-to-set playhead (the arrangement canvas calls this): writes the
    // seek target AND the start position, so a later Play/first-note record
    // begins here — "record from specific times".
    auto& ctrl = audio_.transport_control();
    double const beats_per_bar =
        static_cast<double>(ctrl.meter_num.load(std::memory_order_relaxed));
    double const spb = 60.0 /
                       std::clamp(ctrl.tempo_bpm.load(std::memory_order_relaxed),
                                  20.0, 333.0);
    auto const samples = static_cast<std::uint64_t>(std::llround(
        static_cast<double>(std::max(bar, 0)) * beats_per_bar * spb *
        app_sample_rate()));
    ctrl.start_position.store(samples, std::memory_order_relaxed);
    ctrl.position_req.store(samples, std::memory_order_release);
    ctrl.state_req.store(music::TransportControl::kReqPositionSet,
                         std::memory_order_release);
}

void PlayEngine::set_clip_bars(int track, int bars) {
    if (track < 0 || track >= track_count()) return;
    auto& clip = clips_[static_cast<std::size_t>(track)];
    double const beats_per_bar =
        static_cast<double>(
            audio_.transport_control().meter_num.load(
                std::memory_order_relaxed));
    auto const ticks = static_cast<std::int64_t>(std::llround(
        static_cast<double>(std::max(bars, 1)) * music::kPPQ *
        beats_per_bar));
    clip.length_ticks = ticks;
    // Events recorded against the old length wrap into the new window at
    // their next commit; events now past the end stay stored but don't
    // fire (the player drops offsets past the loop) — shortening then
    // re-lengthening loses nothing. The next save persists the length.
    log::info("app", "clip: track {} -> {} bars", track, bars);
    republish_clips();
}

void PlayEngine::take_cover(int track, std::int64_t now) {
    // App thread: grow the track's clip window to cover the take's span
    // [take_start, now] and record the span as the take's OWN box — the
    // canvas draws one block per take, so a finished take's block stays
    // exactly where it was played (user report: "[rec 1] [rec 2] -> it
    // removes rec 1, we want both"). The old behavior REPLACED the clip's
    // placement on every take, moving — and musically repossessing — the
    // previous recording. Now: the window only ever extends (a take that
    // starts before the window shifts the stored locals right, preserving
    // every event's absolute position), and each take spans its own box.
    if (track < 0 || track >= track_count()) return;
    auto& ctrl = audio_.transport_control();
    double const beats_per_bar =
        static_cast<double>(
            ctrl.meter_num.load(std::memory_order_relaxed));
    double const bar = static_cast<double>(music::kPPQ) * beats_per_bar;
    double const now_t = static_cast<double>(now);
    if (take_start_tick_ < 0) {
        take_start_tick_ =
            static_cast<std::int64_t>(std::floor(now_t / bar) * bar);
    }
    auto const bars = static_cast<std::int64_t>(
                          std::floor((now_t - static_cast<double>(take_start_tick_)) /
                                     bar)) + 1;
    auto const len = std::max<std::int64_t>(
        1, bars * static_cast<std::int64_t>(bar));
    auto& clip = clips_[static_cast<std::size_t>(track)];
    std::int64_t const old_end = clip.start_tick + clip.length_ticks;
    if (take_start_tick_ < clip.start_tick) {
        // Extend the window leftward: shift the stored locals right so
        // each event keeps its absolute position (playback = start + local).
        auto const shift = clip.start_tick - take_start_tick_;
        for (auto& e : clip.events) e.tick += shift;
        clip.start_tick = take_start_tick_;
    }
    // The window must cover the union: the existing content's end (its
    // locals moved with a leftward extension) AND the new take's end.
    auto const window_end = std::max(old_end, take_start_tick_ + len);
    auto const needed = window_end - clip.start_tick;  // local ticks
    if (clip.length_ticks < needed) clip.length_ticks = needed;
    clip.place_take_span(clip.next_take, take_start_tick_, len);
    take_mask_.fetch_or(1u << track, std::memory_order_relaxed);
}

std::vector<mapping::Command> PlayEngine::drain_app_events() {
    // App/Qt thread: mapping + recorder + recording gate.
    std::vector<mapping::Command> all;
    midi::MidiEvent ev{};
    auto& ctrl = audio_.transport_control();
    bool const arp = arp_on_.load(std::memory_order_acquire);
    // Take placement: while recording and rolling, the keys-target clip
    // grows with the playhead (the arrangement renders the growth live —
    // "record and place what you see"). Covered before any note of this
    // drain records, so nothing ever wraps into a stale window.
    if (recording_ &&
        ctrl.play_state.load(std::memory_order_relaxed) != 0) {
        take_cover(selected_track_.load(std::memory_order_relaxed),
                   static_cast<std::int64_t>(now_ticks(
                       audio_.transport_position(), ctrl.tempo_bpm.load(),
                       app_sample_rate())));
    }
    while (event_q_.pop(ev)) {
        switch (ev.type) {
        case midi::Type::NoteOn:
        case midi::Type::NoteOff: {
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
                        // Chord-capture snapshot (grow-only): any key played
                        // while a slot pad is held is remembered for that
                        // slot, so the capture survives releasing the keys
                        // before the pad and quick stabs alike. Arp strikes
                        // (app-generated) never enter the snapshot.
                        for (std::size_t s = 0; s < chord_holding_.size();
                             ++s) {
                            if (chord_holding_[s] &&
                                std::find(chord_capture_[s].begin(),
                                          chord_capture_[s].end(), n) ==
                                    chord_capture_[s].end()) {
                                chord_capture_[s].push_back(n);
                            }
                        }
                    } else {
                        std::erase(chord_held_keys_, n);
                        if (arp) arp_.note_off(n);
                    }
                }
            }
            // Recording — the loop machine. WHAT records WHERE:
            //   pads (channel 9, PLAY mode only, notes 36..51): the Drums
            //     clip, raw note (the Sampler maps bank B's +12 itself);
            //   keys (channel != 9, every mode — they play in every mode):
            //     ANY note 0..127, at the SOUNDING pitch (octave/scale
            //     applied exactly like the live path), into the SELECTED
            //     track's clip, with the curve-applied velocity (what you
            //     heard) and the track's own channel so playback lands on
            //     the instrument that was played. Keys selected on the
            //     Drums track record as their pad note; Bass stays
            //     pitch-faithful (the -12 is a playback transform, same
            //     convention as the octave shift).
            // The old gate recorded keys ONLY in the bottom octave
            // (36..43 — "full-key recording deferred with clip playback"):
            // playing in a normal octave silently recorded nothing, which
            // is the "I can't record the instruments at all" report.
            // Clip playback exists now, so the window is gone.
            int record_track = -1;
            int stored_channel = ev.channel;
            int stored_note = ev.note();
            int stored_velocity = ev.velocity();
            if (ev.channel == 9) {
                // Pads record into the (single, shared-kit) Drums slot.
                record_track = drums_slot();
            } else {
                // Arp-on keys bypass voices on the MIDI thread (only arp
                // strikes sound) — mirror that: nothing to record that
                // didn't sound. Otherwise the play-once twin below must
                // see the same event stream as the MIDI-thread guard, or
                // sound-suppressed re-fires would record doubles.
                if (!arp_on_.load(std::memory_order_acquire) &&
                    record_retrigger_.allow(ev.channel, ev.note(),
                                            ev.is_note_on(), ev.stamp_ns)) {
                    int n = ev.note() + octave_ * 12;
                    if (scale_lock_.load(std::memory_order_acquire)) {
                        n = music::snap_to_scale(*scale_, scale_root_, n);
                    }
                    if (n >= 0 && n <= 127) {
                        int const t = selected_track_.load(
                            std::memory_order_relaxed);
                        record_track = t;
                        if (track_kind(t) == TrackKind::Drums) {
                            int pad = (n - 36) % 8;  // same map as live
                            if (pad < 0) pad += 8;
                            stored_note = 36 + pad;
                            stored_channel = 9;
                        } else {
                            stored_channel = t;  // slot = synth voice identity
                        }
                        stored_velocity = std::min(
                            127,
                            music::apply_velocity_curve(
                                vel_curve_.load(std::memory_order_relaxed),
                                ev.d2));
                    }
                }
            }
            bool const record_note =
                recording_ && record_track >= 0 &&
                channel_track(record_arm_.load(std::memory_order_relaxed),
                              ev.channel);
            if (record_note && !ev.is_note_on()) {
                // Capture all four sizes first: which pending matches (and
                // therefore which clip commits) is decided inside.
                std::array<std::size_t, kTrackCount> before{};
                for (int t = 0; t < kTrackCount; ++t) {
                    before[static_cast<std::size_t>(t)] =
                        clips_[static_cast<std::size_t>(t)].events.size();
                }
                if (music::Clip* committed = recorder_.note_off(
                        static_cast<std::int64_t>(now_ticks(
                            audio_.transport_position(), ctrl.tempo_bpm.load(),
                            app_sample_rate())),
                        ev.channel, ev.note());
                    committed != nullptr) {
                    auto const t = static_cast<int>(committed - clips_.data());
                    journal_new_events(t, before[static_cast<std::size_t>(t)]);
                }
            }
            if (record_note && ev.is_note_on()) {
                // First-note record start: armed while the transport is
                // stopped → it begins rolling NOW (from the playhead), so
                // the take starts with the very note that triggered it.
                // Punch-in (armed while rolling) is unaffected.
                if (ctrl.play_state.load(std::memory_order_relaxed) == 0) {
                    ctrl.state_req.store(music::TransportControl::kReqPlay);
                }
                double const ticks =
                    now_ticks(audio_.transport_position(),
                              ctrl.tempo_bpm.load(std::memory_order_relaxed),
                              app_sample_rate());
                // Place the take's block before the note records, so the
                // note lands inside the grown window (never wrapped into a
                // stale one) — pads cover the Drums clip lazily here, on
                // their first actual note.
                take_cover(record_track,
                           static_cast<std::int64_t>(ticks));
                if (!recorder_.note_on(
                        static_cast<std::int64_t>(ticks), ev.channel,
                        ev.note(), stored_channel, stored_note, stored_velocity,
                        clips_[static_cast<std::size_t>(record_track)])) {
                    log::warn("app", "clip full — take rejected");
                }
            }
            break;
        }
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
    // Keep the RT players in sync with every clip mutation this drain made
    // (note-off commits, flushes) and with tempo/rate moves.
    republish_clips();
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
                  app_sample_rate());
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
        } else {
            apply_voice(vc.track, vc.channel, vc.note, vc.velocity, vc.on);
        }
    }
    // Knob-driven parameter updates: a whole SynthParams per message —
    // knob turns are slow, and struct-at-once beats per-field atomics for
    // both correctness (no half-updated patch) and simplicity.
    ParamCmd pc{};
    while (param_q_.pop(pc)) {
        if (pc.track < track_count() &&
            track_kind(pc.track) != TrackKind::Drums) {
            synths_[static_cast<std::size_t>(pc.track)].set_params(pc.params);
        }
    }
    // FX parameter updates: a whole FxParams per message. FxChain::set_params
    // is copy + clamp + pure math (no allocation) — RT-callable per the
    // fx.hpp contract; applies at this buffer boundary.
    FxParamCmd fc{};
    while (fx_q_.pop(fc)) {
        if (fc.track < track_count()) {
            fx_chains_[static_cast<std::size_t>(fc.track)].set_params(
                fc.params);
        }
    }
    // KIT knob updates: a whole SamplerParams per message (kit gain, last-
    // pad tune/decay). Applied HERE on the RT thread — set_params on the
    // control thread would race the note_on() reads.
    audio::SamplerParams kp{};
    while (kit_q_.pop(kp)) {
        drums_.set_params(kp);
    }
    // Tempo-synced delay lengths: follow the transport tempo (RT-safe pure
    // math per fx.hpp; a change steps the echo once — rare knob action).
    double const bpm_now = audio_.transport_control().tempo_bpm.load(
        std::memory_order_relaxed);
    if (bpm_now != fx_tempo_) {
        fx_tempo_ = bpm_now;
        for (auto& fx : fx_chains_) {
            fx.set_tempo(bpm_now);
        }
    }

    // Clip playback: the recorded takes fire against the transport clock.
    // This hook runs BEFORE Transport::advance inside the callback, so
    // transport_position() is this buffer's start sample and play_state is
    // the previous buffer's state (a stop releases voices one buffer late —
    // inaudible, and the click scheduling behaves the same way). Fires go
    // straight to the instruments: we ARE the RT thread, exactly like the
    // queue drain above. THE piece that makes "record → play" audible —
    // until now clip events were only ever read by the offline exporter.
    std::uint8_t const play_state =
        audio_.transport_control().play_state.load(std::memory_order_relaxed);
    if (play_state != 0) {
        for (int t = 0; t < track_count(); ++t) {
            clip_players_[static_cast<std::size_t>(t)].render(
                audio_.transport_position(), frames,
                [this, t](int channel, int note, int velocity,
                          bool on) noexcept {
                    apply_voice(t, channel, note, velocity, on);
                });
        }
    } else if (last_play_state_ != 0) {
        // Stop edge: note-off (not panic-mute) every note the players left
        // sounding, so release tails decay naturally like a groovebox stop.
        for (int t = 0; t < track_count(); ++t) {
            clip_players_[static_cast<std::size_t>(t)].release_all(
                [this, t](int channel, int note, int velocity,
                          bool on) noexcept {
                    apply_voice(t, channel, note, velocity, on);
                });
        }
    }
    last_play_state_ = play_state;

    // Mix: each synth renders into the pre-allocated scratch (Synth has
    // overwrite semantics; Sampler accumulates — scratch is zeroed for it),
    // its per-track FX chain processes in place, then volume x equal-power
    // pan is applied and the result adds into the engine's zeroed buffers.
    // Chunked so `frames` can never overrun the scratch regardless of the
    // backend's buffer size.
    for (std::uint32_t off = 0; off < frames; off += kMaxBlock) {
        std::uint32_t const n = std::min(kMaxBlock, frames - off);
        float* L = left + off;
        float* R = right + off;
        for (int t = 0; t < track_count(); ++t) {
            auto const ti = static_cast<std::size_t>(t);
            // Mute gate (TrackStrips M buttons): a muted track contributes
            // nothing, but still renders + runs its FX chain so delay and
            // reverb tails keep decaying (unmute = the tail is still there).
            bool const muted = track_mute_[ti].load(std::memory_order_relaxed);
            float const vol = muted ? 0.0f
                                    : track_vol_[ti].load(
                                          std::memory_order_relaxed);
            float const pan =
                track_pan_[ti].load(std::memory_order_relaxed);
            // Equal-power pan: 0 -> full left, 0.5 -> center (-3 dB both),
            // 1 -> full right.
            float const ang = pan * 1.5707963267948966f;  // pi/2
            float const gl = vol * std::cos(ang);
            float const gr = vol * std::sin(ang);
            float* sl = scratch_l_.data();
            float* sr = scratch_r_.data();
            if (track_kind(t) == TrackKind::Drums) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    sl[i] = 0.f;
                    sr[i] = 0.f;
                }
                drums_.render(sl, sr, n);
            } else {
                synths_[ti].render(sl, sr, n);
            }
            // Per-track FX (insert chain, default bypass). Processed even
            // for muted tracks so tails (delay/reverb) keep decaying.
            fx_chains_[ti].process(sl, sr, n);
            if (gl == 0.f && gr == 0.f) continue;  // muted track
            for (std::uint32_t i = 0; i < n; ++i) {
                L[i] += sl[i] * gl;
                R[i] += sr[i] * gr;
            }
        }
    }
    // Live mic: pop whatever the backend captured this buffer, roll the
    // grab-history window with the post-gain signal, and (when monitoring)
    // mix into the master BEFORE the master fader — the mic follows the
    // MasterVolume knob like every other source. A capture underflow (the
    // ring gave fewer frames than the buffer needs) is silence for that
    // stretch; the history only ever holds frames that actually arrived.
    float mic_peak = 0.0f;
    std::uint64_t mic_got = 0;
    for (std::uint32_t off = 0; off < frames; off += kMaxBlock) {
        std::uint32_t const n = std::min(kMaxBlock, frames - off);
        std::uint64_t const got =
            mic_ring_.pop(mic_l_.data(), mic_r_.data(), n);
        mic_got += got;
        if (got == 0) continue;
        float const g = mic_gain_.load(std::memory_order_relaxed);
        for (std::uint64_t i = 0; i < got; ++i) {
            mic_l_[static_cast<std::size_t>(i)] *= g;
            mic_r_[static_cast<std::size_t>(i)] *= g;
            mic_peak = std::max(mic_peak, std::abs(mic_l_[static_cast<std::size_t>(i)]));
            mic_peak = std::max(mic_peak, std::abs(mic_r_[static_cast<std::size_t>(i)]));
        }
        if (mic_history_ != nullptr) {
            mic_history_->push(mic_l_.data(), mic_r_.data(), got);
        }
        if (mic_monitor_.load(std::memory_order_relaxed)) {
            float* L = left + off;
            float* R = right + off;
            for (std::uint64_t i = 0; i < got; ++i) {
                L[i] += mic_l_[static_cast<std::size_t>(i)];
                R[i] += mic_r_[static_cast<std::size_t>(i)];
            }
        }
    }
    mic_level_.store(mic_peak, std::memory_order_relaxed);
    if (mic_got > 0) {
        mic_last_ms_.store(steady_ms(), std::memory_order_relaxed);
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
        if (t < 0 || t >= track_count() || track_kind(t) == TrackKind::Drums) {
            return;
        }
        track_preset_[static_cast<std::size_t>(t)].store(
            -1, std::memory_order_relaxed);  // manual edit -> custom patch
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

    // FX knobs (Edit-mode FX page): they act on the SELECTED track (drums
    // included — the kit benefits from glue EQ/delay) at the current
    // fx_slot_. A/B are slot-dependent (see fx_slot_param_name); values map
    // 0..127 -> musical ranges, delay beats snap to note values by zone.
    auto fx_knob = [&](int ab, int param) {
        int const t = selected_track();
        if (t < 0 || t >= track_count()) {
            return;
        }
        int const slot = fx_slot();
        if (slot < 0 || slot >= kFxSlotCount) {
            return;
        }
        float const x =
            static_cast<float>(std::clamp(param, 0, 127)) / 127.0f;
        track_preset_[static_cast<std::size_t>(t)].store(
            -1, std::memory_order_relaxed);
        auto p = fx_params_[static_cast<std::size_t>(t)];
        switch (static_cast<FxSlot>(slot)) {
        case FxSlot::Eq:
            if (ab == 0) p.eq_low_db = -18.0f + 36.0f * x;
            else p.eq_high_db = -18.0f + 36.0f * x;
            break;
        case FxSlot::Comp:
            if (ab == 0) p.comp_threshold = 0.02f + 0.98f * x;
            else p.comp_ratio = 1.0f + 19.0f * x;
            p.comp_on = true;
            break;
        case FxSlot::Crush:
            if (ab == 0) p.crush_bits = 2.0f + 14.0f * x;
            else p.crush_downsample = 1.0f + 15.0f * x;
            p.crush_on = true;
            break;
        case FxSlot::Drive:
            if (ab == 0) p.drive = 1.0f + 7.0f * x;
            else p.drive_shape = x < 1.0f / 3.0f ? audio::DriveShape::Soft
                                : x < 2.0f / 3.0f ? audio::DriveShape::Hard
                                                  : audio::DriveShape::Fold;
            break;
        case FxSlot::Chorus:
            if (ab == 0) {
                p.chorus_rate_hz = 0.01f + 9.99f * x;
            } else {
                p.chorus_mix = x;
                p.chorus_on = p.chorus_mix > 0.02f;  // wetness enables
            }
            break;
        case FxSlot::Flanger:
            if (ab == 0) {
                p.flanger_rate_hz = 0.05f + 9.95f * x;
            } else {
                p.flanger_mix = x;
                p.flanger_on = p.flanger_mix > 0.02f;
            }
            break;
        case FxSlot::Delay: {
            if (ab == 0) {
                // Five zones: 1/16, 1/8, 1/4, 1/2, 1 note (quartile-ish
                // thresholds — a committed knob position is a committed
                // note value, mirroring the quantize-grid knob).
                p.delay_beats = param < 26  ? 1.0f / 16.0f
                                : param < 51 ? 1.0f / 8.0f
                                : param < 77 ? 0.25f
                                : param < 102 ? 0.5f
                                              : 1.0f;
            } else {
                p.delay_mix = x;
            }
            p.delay_on = p.delay_mix > 0.02f;
            break;
        }
        case FxSlot::Tremolo:
            if (ab == 0) {
                p.tremolo_rate_hz = 0.05f + 19.95f * x;
            } else {
                p.tremolo_depth = x;
                p.tremolo_on = p.tremolo_depth > 0.02f;
            }
            break;
        case FxSlot::Reverb:
            if (ab == 0) {
                p.reverb_decay = x;
            } else {
                p.reverb_mix = x;
                p.reverb_on = p.reverb_mix > 0.001f;
            }
            break;
        }
        set_track_fx_params(t, p);
    };

    // FX TOGGLE: flips the selected slot's on/off on the selected track.
    // EQ has no on-flag — toggling zeroes the bands (off) or applies a
    // mild tilt (+3 dB low and high). Wet-effect toggles raise a usable
    // default mix when switching on from a ~zero value.
    auto fx_toggle = [&]() {
        int const t = selected_track();
        if (t < 0 || t >= track_count()) {
            return;
        }
        int const slot = fx_slot();
        if (slot < 0 || slot >= kFxSlotCount) {
            return;
        }
        track_preset_[static_cast<std::size_t>(t)].store(
            -1, std::memory_order_relaxed);
        auto p = fx_params_[static_cast<std::size_t>(t)];
        switch (static_cast<FxSlot>(slot)) {
        case FxSlot::Eq:
            if (p.eq_low_db != 0.f || p.eq_mid_db != 0.f ||
                p.eq_high_db != 0.f) {
                p.eq_low_db = p.eq_mid_db = p.eq_high_db = 0.f;
            } else {
                p.eq_low_db = 3.f;
                p.eq_high_db = 3.f;
            }
            break;
        case FxSlot::Comp:
            p.comp_on = !p.comp_on;
            break;
        case FxSlot::Crush:
            p.crush_on = !p.crush_on;
            break;
        case FxSlot::Drive:
            p.drive = p.drive > 1.0005f ? 1.0f : 2.5f;
            break;
        case FxSlot::Chorus:
            p.chorus_on = !p.chorus_on;
            if (p.chorus_on && p.chorus_mix < 0.05f) p.chorus_mix = 0.3f;
            break;
        case FxSlot::Flanger:
            p.flanger_on = !p.flanger_on;
            if (p.flanger_on && p.flanger_mix < 0.05f) p.flanger_mix = 0.5f;
            break;
        case FxSlot::Delay:
            p.delay_on = !p.delay_on;
            if (p.delay_on && p.delay_mix < 0.05f) p.delay_mix = 0.25f;
            break;
        case FxSlot::Tremolo:
            p.tremolo_on = !p.tremolo_on;
            if (p.tremolo_on && p.tremolo_depth < 0.05f) p.tremolo_depth = 0.5f;
            break;
        case FxSlot::Reverb:
            p.reverb_on = !p.reverb_on;
            if (p.reverb_on && p.reverb_mix < 0.02f) p.reverb_mix = 0.25f;
            // (the send-knob mirror is synced by set_track_fx_params)
            break;
        }
        set_track_fx_params(t, p);
    };

    switch (c.action) {
    case A::TransportPlay:
        // Metronome is OPT-IN (user report: "remove metronome
        // automatically on playing — it's annoying, it should be
        // selectable"): only the M button / MetronomeToggle touches the
        // flag now. Play begins at the playhead (start_position).
        ctrl.state_req.store(music::TransportControl::kReqPlay);
        break;
    case A::TransportStop: {
        ctrl.state_req.store(music::TransportControl::kReqStop);
        std::array<std::size_t, kTrackCount> before{};
        int const n_tracks = track_count();
        for (int t = 0; t < n_tracks; ++t) {
            before[static_cast<std::size_t>(t)] =
                clips_[static_cast<std::size_t>(t)].events.size();
        }
        recording_ = false;  // stop also disarms — no ghost-armed captures
        recorder_.flush_pending();
        // Take finalized: each recorded take keeps its own box (its span);
        // a take that never played a note (armed while rolling, nothing
        // played) leaves no ghost block.
        for (auto& cl : clips_) {
            cl.prune_empty_boxes();
        }
        take_start_tick_ = -1;
        take_mask_.store(0, std::memory_order_relaxed);
        for (int t = 0; t < n_tracks; ++t) {
            journal_new_events(t, before[static_cast<std::size_t>(t)]);
        }
        std::string err;
        if (!save_project(err)) {
            log::error("app", "project save failed: {}", err);
        }
        break;
    }
    case A::TransportRecord:
        // ARM, not roll (user report: "recording should be done by user
        // playing"): the transport does NOT start here. If it is already
        // rolling this is a punch-in at the playhead; if stopped, the
        // FIRST recorded note-on starts it (see the record gate in
        // drain_app_events). No kReqToggleRecord — the RT Recording state
        // was only ever a toggle bookkeeping flag.
        recording_ = !recording_;
        if (recording_) {
            // Fresh take session: stale redo entries die here (a new take
            // would otherwise restore under a reused take index), and the
            // record-side play-once guard starts from a clean slate. Take
            // placement starts from scratch too.
            for (auto& cl : clips_) {
                cl.clear_redo();
            }
            record_retrigger_.clear();
            take_start_tick_ = -1;
            take_mask_.store(0, std::memory_order_relaxed);
        } else {
            // Punch-out: finalize the placed takes (each keeps its box).
            recorder_.flush_pending();
            for (auto& cl : clips_) {
                cl.prune_empty_boxes();
            }
            take_start_tick_ = -1;
            take_mask_.store(0, std::memory_order_relaxed);
        }
        break;
    case A::MetronomeToggle:
        ctrl.metronome_on.store(!ctrl.metronome_on.load());
        break;
    case A::CountInToggle:
        ctrl.count_in_bars.store(!ctrl.count_in_bars.load());
        break;
    case A::LoopToggle: {
        // Flip between no loop and the last-used length (knob 6 / LoopBarsSet
        // sets it). Default restore length: 4 bars.
        std::uint64_t const cur = ctrl.loop_bars.load();
        if (cur != 0) {
            last_loop_bars_ = static_cast<int>(
                std::clamp<std::uint64_t>(cur, 1, 127));
            ctrl.loop_bars.store(0);
        } else {
            ctrl.loop_bars.store(static_cast<std::uint64_t>(
                std::clamp(last_loop_bars_, 1, 127)));
        }
        break;
    }
    case A::Undo: {
        // Bank-B B1. One info line per press — undo/redo are silent
        // operations otherwise, and "pad did nothing" reports have to be
        // answerable from the log. Loop machine: undo applies to the
        // SELECTED track's clip (the take you just recorded into it).
        auto& clip = clips_[static_cast<std::size_t>(selected_track())];
        int const take = clip.next_take;
        if (clip.undo_last_take()) {
            log::info("app", "clip: undid take {} ({} left)", take,
                      clip.next_take);
        } else {
            log::info("app", "clip: nothing to undo");
        }
        break;
    }
    case A::Redo: {
        // Bank-B B2. Was silently dropped before (user report: "BANK B
        // doesn't work at all") — the mapping emitted the action but no
        // case consumed it and Clip had no redo stack.
        auto& clip = clips_[static_cast<std::size_t>(selected_track())];
        if (clip.redo_last_take()) {
            log::info("app", "clip: redid take {} ({} in clip)",
                      clip.next_take - 1, clip.next_take);
        } else {
            log::info("app", "clip: nothing to redo");
        }
        break;
    }
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
    case A::TrackSend: {
        // The send knob IS the track's reverb amount now: 0..127 -> reverb
        // mix 0..1 on the selected track's insert chain (dry stays full).
        // App truth + RT push + the UI mirror atomic all move together.
        int const t = selected_track();
        if (t < 0 || t >= track_count()) break;
        float const mix = static_cast<float>(std::clamp(c.param, 0, 127)) / 127.0f;
        auto p = fx_params_[static_cast<std::size_t>(t)];
        p.reverb_mix = mix;
        p.reverb_on = mix > 0.001f;
        set_track_fx_params(t, p);
        break;
    }
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
    case A::VelocityCurveSet:
        // KIT knob 1, four zones: linear / log (the shipped default) /
        // exp / hard-floor. Applied by the MIDI thread per note-on.
        vel_curve_.store(
            c.param < 32  ? music::VelocityCurve::Linear
            : c.param < 64 ? music::VelocityCurve::Log
            : c.param < 96 ? music::VelocityCurve::Exp
                           : music::VelocityCurve::HardFloor,
            std::memory_order_relaxed);
        break;
    case A::KitGain:
        // KIT knob 2: kit-wide sample gain 0..2 (unity sits at ~64).
        kit_params_.gain =
            2.0f * static_cast<float>(std::clamp(c.param, 0, 127)) / 127.0f;
        if (!kit_q_.push(kit_params_)) {
            log::warn("app", "kit queue full — sampler params dropped");
        }
        break;
    case A::PadTune: {
        // KIT knob 3: the LAST pad you hit (PLAY mode hit or KIT audition),
        // -12..+12 semitones, natural pitch at ~64.
        int const st = static_cast<int>(std::lround(
                           static_cast<double>(std::clamp(c.param, 0, 127)) /
                           127.0 * 24.0)) -
                       12;
        kit_params_.pad_tuning_semitones[static_cast<std::size_t>(
            last_drum_pad_)] = st;
        if (!kit_q_.push(kit_params_)) {
            log::warn("app", "kit queue full — sampler params dropped");
        }
        break;
    }
    case A::PadDecay: {
        // KIT knob 4: the last pad's decay scale 0.1..4.0 (exponential —
        // time constants are logarithmic to the ear; 1.0 = natural decay).
        double const t = static_cast<double>(std::clamp(c.param, 0, 127)) /
                         127.0;
        kit_params_.pad_decay_scale[static_cast<std::size_t>(
            last_drum_pad_)] =
            static_cast<float>(0.1 * std::pow(40.0, t));
        if (!kit_q_.push(kit_params_)) {
            log::warn("app", "kit queue full — sampler params dropped");
        }
        break;
    }
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
    case A::FxSelect:
        // Knob picks the FX slot: 9 zones across the sweep (a committed
        // knob position is a committed slot; the panel + mirror follow).
        set_fx_slot(std::clamp(c.param, 0, 127) * kFxSlotCount / 128);
        break;
    case A::FxToggle:
        fx_toggle();
        break;
    case A::FxParamA:
        fx_knob(0, c.param);
        break;
    case A::FxParamB:
        fx_knob(1, c.param);
        break;
    case A::TrackSelect:
        select_track(c.param);
        break;
    case A::DrumPad:
        // Sound came via the RT queue; recorder records in drain. The pad
        // index also becomes the KIT tune/decay knob target (bank B is the
        // same pad one octave up, so absolute 0..15 collapses to 0..7).
        last_drum_pad_ = ((c.param % 8) + 8) % 8;
        break;
    case A::ChordSlot: {
        int const slot = c.param;
        if (slot < 0 || slot >= kChordSlots) break;
        if (c.delta == 0) {
            chord_press_[static_cast<std::size_t>(slot)] =
                std::chrono::steady_clock::now();
            chord_holding_[static_cast<std::size_t>(slot)] = true;
            chord_capture_[static_cast<std::size_t>(slot)].clear();
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
            } else if (chord.empty()) {
                // Empty slot: silence IS the response, but it must never be
                // an unexplained silence — one line per slot per process
                // (same courtesy as unmapped CCs) tells the user how to
                // fill it. This was the "BANK B doesn't work at all" trap:
                // B3..B8 look dead until a chord has been captured.
                static bool logged_empty[kChordSlots] = {};
                if (!logged_empty[slot]) {
                    logged_empty[slot] = true;
                    log::info("app", "chord slot {} empty — hold the pad, "
                                     "play keys, release the pad to capture",
                              slot);
                }
            }
        } else {
            chord_holding_[static_cast<std::size_t>(slot)] = false;
            auto const held_ms = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                           chord_press_[static_cast<
                                               std::size_t>(slot)])
                .count();
            if (held_ms >= 400 && !chord_capture_[static_cast<std::size_t>(
                                      slot)]
                                      .empty()) {
                // Capture: hold slot + play keys any time during the hold
                // -> store the chord. The grow-only snapshot (fed in
                // drain_app_events) survives releasing the keys before the
                // pad — the natural gesture order — and quick stabs too
                // (the old code captured only keys still held at release).
                auto const& cap =
                    chord_capture_[static_cast<std::size_t>(slot)];
                chord_slots_[static_cast<std::size_t>(slot)] = cap;
                log::info("app", "chord slot {} captured ({} notes)", slot,
                          cap.size());
            }
            chord_capture_[static_cast<std::size_t>(slot)].clear();
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
    case A::ModeEdit: set_mode(mapping::Mode::Edit); break;
    case A::ModeStep: set_mode(mapping::Mode::Step); break;
    case A::ModePlay: set_mode(mapping::Mode::Play); break;
    case A::PadPackSelect: {
        // KIT mode pads: load the pack, then audition that pad's voice on
        // the new kit — "no other pack on pad working" now answers itself
        // by ear the instant a pad is pressed. param = pack 0..15.
        int const pack = std::clamp(c.param, 0, audio::DrumKit::pack_count() - 1);
        std::string err;
        if (set_pad_pack(pack, err)) {
            log::info("app", "pad pack: {} ({})", audio::DrumKit::pack_name(pack),
                      pack);
            // Audition: the kit sound this pad maps to (pad = pack % 8).
            // The auditioned pad is also the KIT tune/decay knob target.
            last_drum_pad_ = pack % 8;
            push_voice(2, 36 + pack % 8, 100, true);
        } else {
            log::error("app", "pad pack {} failed: {}", pack, err);
        }
        break;
    }
    case A::ArpToggle: set_arp(!arp_on()); break;
    case A::ScaleLockToggle:
        scale_lock_.store(!scale_lock(), std::memory_order_release);
        break;
    case A::KeysPlayOnceToggle:
        set_keys_play_once(!keys_play_once());
        break;
    case A::OctaveUp: octave_ = std::min(octave_ + 1, 4); break;
    case A::OctaveDown: octave_ = std::max(octave_ - 1, -4); break;
    default:
        break;
    }
}

// --- track API ------------------------------------------------------------

char const* PlayEngine::track_name(int idx) const noexcept {
    if (idx < 0 || idx >= track_count()) return "";
    return slots_[static_cast<std::size_t>(idx)].name.c_str();
}

TrackKind PlayEngine::track_kind(int idx) const noexcept {
    if (idx < 0 || idx >= track_count()) return TrackKind::Synth;
    return static_cast<TrackKind>(
        slots_[static_cast<std::size_t>(idx)].kind.load(
            std::memory_order_relaxed));
}

int PlayEngine::drums_slot() const noexcept {
    for (int t = 0; t < track_count(); ++t) {
        if (track_kind(t) == TrackKind::Drums) return t;
    }
    return -1;
}

int PlayEngine::channel_track(unsigned arm_mask, int channel) const noexcept {
    // Record-arm gate (TrackStrips rec dots): with no track armed,
    // everything live records (the shipped behavior); with any armed, only
    // notes from armed tracks land in a clip. Channel -> slot: synth slots
    // own channel == slot index; the drums slot owns channel 9.
    if (arm_mask == 0) return channel >= 0 ? 1 : -1;  // gate passes
    int const t = channel == 9 ? drums_slot() : channel;
    return t >= 0 && t < track_count() && ((arm_mask >> t) & 1u) != 0 ? t : -1;
}

bool PlayEngine::add_track(int preset_index, std::string& error) {
    // Drum Kit shares ONE kit instance (~12 MB of synthesized packs) — a
    // second drums track would fight over the active pack for no gain.
    bool const drums = preset_index < 0;
    if (drums && drums_slot() >= 0) {
        error = "a Drums track already exists (the kit is shared)";
        return false;
    }
    int const t = track_count();
    if (t >= kTrackCount) {
        error = "track limit reached (8)";
        return false;
    }
    auto const ti = static_cast<std::size_t>(t);
    // Activate the slot FIRST: the RT drains guard on track < track_count()
    // — pushing patch/FX params before the slot is active would let a
    // buffer render in between and silently drop them.
    active_.store(t + 1, std::memory_order_release);
    track_vol_[ti].store(1.0f, std::memory_order_relaxed);
    track_pan_[ti].store(0.5f, std::memory_order_relaxed);
    track_send_[ti].store(0.0f, std::memory_order_relaxed);
    track_mute_[ti].store(false, std::memory_order_relaxed);
    clips_[ti] = music::Clip{};
    clips_[ti].length_ticks = 96 * 4 * 4;
    if (drums) {
        slots_[ti].kind.store(static_cast<std::uint8_t>(TrackKind::Drums),
                              std::memory_order_relaxed);
        slots_[ti].name = "Drums";
        slots_[ti].transpose.store(0, std::memory_order_relaxed);
        track_preset_[ti].store(-1, std::memory_order_relaxed);
    } else {
        if (preset_index < 0 ||
            preset_index >= static_cast<int>(presets().size())) {
            // Roll the activation back — the slot never existed.
            active_.store(t, std::memory_order_release);
            error = "bad preset index";
            return false;
        }
        auto const& preset =
            kPresetTable[static_cast<std::size_t>(preset_index)];
        slots_[ti].kind.store(static_cast<std::uint8_t>(TrackKind::Synth),
                              std::memory_order_relaxed);
        slots_[ti].name = preset.name;
        slots_[ti].transpose.store(0, std::memory_order_relaxed);
        // The preset IS the instrument template: patch + FX chain + the
        // preset marker (reuse apply_preset's plumbing).
        synth_params_[ti] = preset.synth;
        synths_[ti].set_params(preset.synth);
        if (!param_q_.push({static_cast<std::uint8_t>(t), preset.synth})) {
            log::warn("app", "param queue full — patch lands next buffer");
        }
        set_track_fx_params(t, preset.fx);
        track_preset_[ti].store(preset_index, std::memory_order_relaxed);
    }
    select_track(t);
    // Journal the structure (dormant ops finally in use): the name rides
    // AddTrack, the kind rides SetTrackParam instrument=.
    if (journal_open_) {
        project::JournalEntry entry;
        entry.op = project::JournalOp::AddTrack;
        entry.s = slots_[ti].name;
        journal_.append(entry);
        project::JournalEntry kind_entry;
        kind_entry.op = project::JournalOp::SetTrackParam;
        kind_entry.i = t;
        kind_entry.s = drums ? "instrument=drumkit" : "instrument=synth";
        journal_.append(kind_entry);
    }
    republish_clips();
    log::info("app", "track added: {} ({})", slots_[ti].name,
              drums ? "drums" : "synth");
    return true;
}

bool PlayEngine::remove_track(int idx, std::string& error) {
    if (idx < 0 || idx >= track_count()) {
        error = "track index out of range";
        return false;
    }
    // No capture may be pending into the slot that is about to move.
    recorder_.flush_pending();
    int const n = track_count();
    // Shift first, push params, THEN shrink the active count — the RT
    // param drains guard on track < track_count(), so pushes for the
    // shifted slots must happen while they are still in range.
    for (int t = idx; t < n - 1; ++t) {
        auto const dst = static_cast<std::size_t>(t);
        auto const src = static_cast<std::size_t>(t + 1);
        // Atomics are not copyable — load/store the shift.
        slots_[dst].kind.store(slots_[src].kind.load(
            std::memory_order_relaxed), std::memory_order_relaxed);
        slots_[dst].transpose.store(slots_[src].transpose.load(
            std::memory_order_relaxed), std::memory_order_relaxed);
        slots_[dst].name = slots_[src].name;
        track_vol_[dst].store(track_vol_[src].load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        track_pan_[dst].store(track_pan_[src].load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        track_send_[dst].store(track_send_[src].load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        track_mute_[dst].store(track_mute_[src].load(std::memory_order_relaxed),
                               std::memory_order_relaxed);
        track_preset_[dst].store(track_preset_[src].load(
            std::memory_order_relaxed), std::memory_order_relaxed);
        synth_params_[dst] = synth_params_[src];
        fx_params_[dst] = fx_params_[src];
        clips_[dst] = std::move(clips_[src]);
        if (!param_q_.push(
                {static_cast<std::uint8_t>(t), synth_params_[dst]})) {
            log::warn("app", "param queue full — patch lands next buffer");
        }
        set_track_fx_params(t, fx_params_[dst]);
    }
    // Now shrink: from the RT's view the removed slot simply stops
    // rendering (kind/count gates) and the shifted slots carry the params
    // pushed above.
    active_.store(n - 1, std::memory_order_release);
    if (selected_track() >= n - 1) {
        select_track(std::max(0, n - 2));
    }
    // Shift the arm mask bits above the removed slot down.
    unsigned const mask = record_arm_.load(std::memory_order_relaxed);
    unsigned const lower = mask & ((1u << idx) - 1u);
    unsigned const upper = (mask >> (idx + 1)) << idx;
    record_arm_.store(lower | upper, std::memory_order_relaxed);
    if (journal_open_) {
        project::JournalEntry entry;
        entry.op = project::JournalOp::RemoveTrack;
        entry.i = idx;
        journal_.append(entry);
    }
    republish_clips();
    log::info("app", "track removed: slot {}", idx);
    return true;
}

void PlayEngine::set_clip_start(int track, int bar) {
    if (track < 0 || track >= track_count()) return;
    auto& clip = clips_[static_cast<std::size_t>(track)];
    double const beats_per_bar =
        static_cast<double>(
            audio_.transport_control().meter_num.load(
                std::memory_order_relaxed));
    auto const ticks = static_cast<std::int64_t>(std::llround(
        static_cast<double>(std::max(bar, 0)) * music::kPPQ *
        beats_per_bar));
    auto const delta = ticks - clip.start_tick;
    clip.start_tick = ticks;
    // Drag-to-move relocates the whole take-group: the boxes ride the
    // window (notes are clip-local — they move with it by definition).
    for (auto& b : clip.boxes) b.start_tick += delta;
    if (journal_open_) {
        project::JournalEntry entry;
        entry.op = project::JournalOp::ClipMove;
        entry.i = track;
        entry.f = static_cast<double>(ticks);
        journal_.append(entry);
    }
    republish_clips();
}

void PlayEngine::set_box_bars(int track, int take, int bars) {
    if (track < 0 || track >= track_count() || bars < 1) return;
    auto& clip = clips_[static_cast<std::size_t>(track)];
    double const beats_per_bar =
        static_cast<double>(
            audio_.transport_control().meter_num.load(
                std::memory_order_relaxed));
    auto const bar_ticks = static_cast<std::int64_t>(
        std::llround(static_cast<double>(music::kPPQ) * beats_per_bar));
    for (auto& b : clip.boxes) {
        if (b.take != take) continue;
        b.length_ticks = static_cast<std::int64_t>(bars) * bar_ticks;
        // The take IS the box: events recorded past the new end go with
        // the trimmed span (locals, relative to the window start).
        auto const box_end = b.start_tick + b.length_ticks - clip.start_tick;
        clip.events.erase(
            std::remove_if(clip.events.begin(), clip.events.end(),
                           [&](music::ClipEvent const& e) {
                               return e.take == take && e.tick >= box_end;
                           }),
            clip.events.end());
        // A grown box may reach past the loop window — the window wraps
        // every commit, so it must cover the box.
        auto const needed = b.start_tick + b.length_ticks - clip.start_tick;
        if (clip.length_ticks < needed) clip.length_ticks = needed;
        republish_clips();
        return;
    }
}

void PlayEngine::clear_take(int track, int take) {
    if (track < 0 || track >= track_count()) return;
    auto& clip = clips_[static_cast<std::size_t>(track)];
    if (clip.events.empty() && clip.boxes.empty()) return;
    clip.clear_take(take);
    // Deliberately NOT journaled: the only clip-clear journal op
    // (ClipClear) replays as a whole-clip wipe — a crash recovery would
    // erase the track's OTHER takes, which is more destructive than the
    // honest alternative (the cleared take reappears after recovery).
    republish_clips();
    log::info("app", "take cleared: track {} take {}", track, take);
}

std::vector<PlayEngine::ClipBoxView> PlayEngine::clip_boxes(int track) {
    std::vector<ClipBoxView> out;
    if (track < 0 || track >= track_count()) return out;
    auto const& cl = clip(track);
    double const beats_per_bar =
        static_cast<double>(
            audio_.transport_control().meter_num.load(
                std::memory_order_relaxed));
    double const bar_ticks =
        static_cast<double>(music::kPPQ) * beats_per_bar;
    out.reserve(cl.boxes.size());
    for (auto const& b : cl.boxes) {
        auto const local0 = b.start_tick - cl.start_tick;
        auto const local1 = local0 + b.length_ticks;
        bool has_notes = false;
        for (auto const& e : cl.events) {
            if (e.tick >= local0 && e.tick < local1) {
                has_notes = true;
                break;
            }
        }
        out.push_back(ClipBoxView{
            b.take,
            std::max(0, static_cast<int>(std::llround(
                            static_cast<double>(b.start_tick) /
                            bar_ticks))),
            std::max(1, static_cast<int>(std::llround(
                            static_cast<double>(b.length_ticks) /
                            bar_ticks))),
            has_notes});
    }
    return out;
}

void PlayEngine::clear_clip(int track) {
    if (track < 0 || track >= track_count()) return;
    auto& clip = clips_[static_cast<std::size_t>(track)];
    if (clip.events.empty()) return;
    clip.clear();  // placement (start_tick) survives — only notes go
    if (journal_open_) {
        project::JournalEntry entry;
        entry.op = project::JournalOp::ClipClear;
        entry.i = track;
        entry.f = 0;
        journal_.append(entry);
    }
    republish_clips();
    log::info("app", "clip cleared: track {}", track);
}

void PlayEngine::select_track(int idx) noexcept {
    if (track_count() <= 0) {
        selected_track_.store(0, std::memory_order_relaxed);
        return;
    }
    idx = std::clamp(idx, 0, track_count() - 1);
    // Relaxed: per-note routing reads this; the only race is one note
    // landing on the previous track at the instant of switching — harmless.
    selected_track_.store(idx, std::memory_order_relaxed);
}

void PlayEngine::set_mode(mapping::Mode m) noexcept {
    mapping_.set_mode(m);
    // The MIDI thread gates the pad-drum path on this: PLAY fires all 16
    // pads, EDIT/KIT pads have their mode functions and stay quiet. A
    // one-note race at the switch instant (a pad drumming in the old mode)
    // is harmless — same contract as selected_track_.
    mode_rt_.store(static_cast<int>(m), std::memory_order_relaxed);
}

// --- mixer setters (per-index; TrackStrips UI) ------------------------------

void PlayEngine::set_track_volume(int idx, float v) noexcept {
    if (idx < 0 || idx >= track_count()) return;
    track_vol_[static_cast<std::size_t>(idx)].store(
        std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PlayEngine::set_track_pan(int idx, float v) noexcept {
    if (idx < 0 || idx >= track_count()) return;
    track_pan_[static_cast<std::size_t>(idx)].store(
        std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PlayEngine::set_track_mute(int idx, bool on) noexcept {
    if (idx < 0 || idx >= track_count()) return;
    track_mute_[static_cast<std::size_t>(idx)].store(on,
                                                     std::memory_order_relaxed);
}

void PlayEngine::set_record_arm(int idx, bool on) noexcept {
    if (idx < 0 || idx >= track_count()) return;
    unsigned const bit = 1u << static_cast<unsigned>(idx);
    unsigned mask = record_arm_.load(std::memory_order_relaxed);
    mask = on ? (mask | bit) : (mask & ~bit);
    record_arm_.store(mask, std::memory_order_relaxed);
}

// --- mic / live input -------------------------------------------------------

void PlayEngine::set_mic_gain(float g) noexcept {
    mic_gain_.store(std::clamp(g, 0.0f, 2.0f), std::memory_order_relaxed);
}

bool PlayEngine::mic_active() const noexcept {
    std::int64_t const last = mic_last_ms_.load(std::memory_order_relaxed);
    if (last == 0) return false;
    // ~14 buffers of silence (512-frame buffers at 48 kHz) = the capture
    // path is not delivering (no device / stream dead) — age the stamp out.
    return (steady_ms() - last) < 700;
}

bool PlayEngine::test_feed_mic(float const* l, float const* r,
                               std::uint32_t n) noexcept {
    mic_ring_.push(l, r, n);
    return true;
}

bool PlayEngine::grab_mic_to_pad(int pad, double seconds, std::string& error) {
    if (pad < 0 || pad >= 8) {
        error = "grab: pad 1..8 only (bank A)";
        return false;
    }
    if (mic_history_ == nullptr) {
        error = "grab: capture history unavailable";
        return false;
    }
    double const sr = static_cast<double>(audio_.stats().sample_rate);
    if (sr < 8000.0) {
        error = "grab: sample rate unknown";
        return false;
    }
    std::uint64_t const frames = std::min<std::uint64_t>(
        mic_history_->capacity(),
        static_cast<std::uint64_t>(std::clamp(seconds, 0.1, 8.0) * sr));
    std::vector<float> l(frames);
    std::vector<float> r(frames);
    std::uint64_t const got = mic_history_->read_last(l.data(), r.data(), frames);
    constexpr std::uint64_t kMinFrames = 2400;  // 50 ms @48k: refuse stubs
    if (got < kMinFrames) {
        error = "grab: not enough mic audio captured yet";
        return false;
    }
    // Mono downmix + peak normalize to 0.9 — a grab should be audible on
    // the pad without external gain staging (boost capped at 4x so a
    // near-silent window does not amplify the noise floor into a roar).
    float peak = 0.0f;
    std::vector<float> mono(got);
    for (std::uint64_t i = 0; i < got; ++i) {
        float const m = 0.5f * (l[static_cast<std::size_t>(i)] +
                                r[static_cast<std::size_t>(i)]);
        mono[static_cast<std::size_t>(i)] = m;
        peak = std::max(peak, std::abs(m));
    }
    if (peak < 0.001f) {
        error = "grab: captured audio is silent";
        return false;
    }
    float const norm = std::min(0.9f / peak, 4.0f);
    for (auto& s : mono) {
        s *= norm;
    }
    audio::WavData wav;
    wav.sample_rate = static_cast<std::uint32_t>(sr);
    wav.channels = 1;
    wav.samples = std::move(mono);
    if (!drums_.load_override_data(36 + pad, wav, error)) {
        error = "grab: " + error;
        return false;
    }
    log::info("app", "mic grab: {} s -> pad {} (note {}), {} frames",
              std::clamp(seconds, 0.1, 8.0), pad + 1, 36 + pad, got);
    return true;
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

// --- per-track FX -----------------------------------------------------------

char const* PlayEngine::fx_slot_name(int slot) noexcept {
    static char const* const kNames[kFxSlotCount] = {
        "EQ", "Comp", "Crush", "Drive", "Chorus", "Flanger", "Delay", "Trem",
        "Verb"};
    return slot >= 0 && slot < kFxSlotCount
               ? kNames[static_cast<std::size_t>(slot)]
               : nullptr;
}

char const* PlayEngine::fx_slot_param_name(int slot, int ab) noexcept {
    // Slot-dependent knob labels — keep in sync with the fx_knob mapping in
    // command() (A = ab 0, B = ab 1). Full words where the 62 px lane label
    // fits; the one-line definition for the whole slot is fx_slot_desc().
    switch (static_cast<FxSlot>(slot >= 0 && slot < kFxSlotCount ? slot : 0)) {
    case FxSlot::Eq: return ab == 0 ? "low dB" : "high dB";
    case FxSlot::Comp: return ab == 0 ? "threshold" : "ratio";
    case FxSlot::Crush: return ab == 0 ? "bits" : "downsample";
    case FxSlot::Drive: return ab == 0 ? "amount" : "shape";
    case FxSlot::Chorus: return ab == 0 ? "rate" : "mix";
    case FxSlot::Flanger: return ab == 0 ? "rate" : "mix";
    case FxSlot::Delay: return ab == 0 ? "beats" : "mix";
    case FxSlot::Tremolo: return ab == 0 ? "rate" : "depth";
    case FxSlot::Reverb: return ab == 0 ? "decay" : "mix";
    }
    return nullptr;
}

char const* PlayEngine::fx_slot_desc(int slot) noexcept {
    // One plain-language line per chain slot, shown in the FX panel's
    // description strip. "What does this do to my sound", not the DSP.
    switch (static_cast<FxSlot>(slot >= 0 && slot < kFxSlotCount ? slot : 0)) {
    case FxSlot::Eq:
        return "EQ — tilts the tone: boosts or trims the low and high ends";
    case FxSlot::Comp:
        return "Compressor — evens out volume, quiet parts up, peaks down";
    case FxSlot::Crush:
        return "Bitcrusher — gritty retro lo-fi by quantizing and "
               "stepping the sound";
    case FxSlot::Drive:
        return "Drive — saturation for warmth and grit, harder = more "
               "growl";
    case FxSlot::Chorus:
        return "Chorus — thickens with a shimmering doubled layer";
    case FxSlot::Flanger:
        return "Flanger — sweeping jet-plane whoosh over the sound";
    case FxSlot::Delay:
        return "Delay — echoes the sound in tempo (beats between repeats)";
    case FxSlot::Tremolo:
        return "Tremolo — rhythmic volume wobble at the set rate";
    case FxSlot::Reverb:
        return "Reverb — room ambience; decay sets how long the tail rings";
    }
    return nullptr;
}

void PlayEngine::set_track_fx_params(int track, audio::FxParams const& p) {
    if (track < 0 || track >= track_count()) {
        return;
    }
    auto const ti = static_cast<std::size_t>(track);
    fx_params_[ti] = p;  // app truth
    // Keep the send-knob mirror coherent with whatever reverb amount the
    // params carry (knob turns and preset applies both land here).
    track_send_[ti].store(p.reverb_mix, std::memory_order_relaxed);
    FxParamCmd cmd;
    cmd.track = static_cast<std::uint8_t>(track);
    cmd.params = p;
    (void)fx_q_.push(cmd);  // RT applies at the next buffer boundary
}

// --- presets ----------------------------------------------------------------

std::vector<PlayEngine::PresetInfo> const& PlayEngine::presets() const
    noexcept {
    // Built once from the static table below (names are stable string
    // literals); the vector never mutates afterwards.
    static std::vector<PresetInfo> const kList = [] {
        std::vector<PresetInfo> v;
        v.reserve(kPresetCount);
        for (int i = 0; i < kPresetCount; ++i) {
            v.push_back(PresetInfo{i, kPresetTable[static_cast<std::size_t>(i)].name});
        }
        return v;
    }();
    return kList;
}

bool PlayEngine::apply_preset(int index, int track, std::string& error) {
    error.clear();
    if (index < 0 || index >= kPresetCount) {
        error = "preset index out of range";
        return false;
    }
    if (track < 0 || track >= track_count()) {
        error = "track index out of range";
        return false;
    }
    if (track_kind(track) == TrackKind::Drums) {
        error = "drums have packs, not synth presets";
        return false;
    }
    auto const ti = static_cast<std::size_t>(track);
    PlayEnginePreset const& pr = kPresetTable[static_cast<std::size_t>(index)];
    // Synth patch: app truth + RT push (same path the knobs use).
    synth_params_[ti] = pr.synth;
    ParamCmd pc;
    pc.track = static_cast<std::uint8_t>(track);
    pc.params = pr.synth;
    (void)param_q_.push(pc);
    // FX chain: set_track_fx_params syncs the send mirror + pushes RT.
    set_track_fx_params(track, pr.fx);
    track_preset_[ti].store(index, std::memory_order_relaxed);
    return true;
}

}  // namespace ghost_story::app
