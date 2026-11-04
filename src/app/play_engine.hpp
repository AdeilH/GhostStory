// addu94 — src/app/play_engine.hpp
//
// PlayEngine (M3/M4): the application core.
//
// Threading architecture (prime-directive compliant):
//   MIDI input thread: pushes into two lock-free queues —
//     synth_q_  : consumed by the AUDIO callback each buffer (voices, params)
//     event_q_  : drained by the app/Qt thread (mapping, recorder, UI)
//   Audio callback: drains both queues, renders the 4-track mix. No locks.
//   App/Qt thread: mapping decisions, recorder, transport commands, UI.
//
// Track model (user report: "keys seem to stick on one output for
// everything" — keys always hit the single synth regardless of the selected
// track). Four tracks, each with its own sound; the keys ALWAYS play the
// SELECTED track:
//   0 "Keys A"  dedicated Synth (channel 0), the shipped default patch
//   1 "Keys B"  dedicated Synth (channel 1), a distinct square-lead patch
//   2 "Drums"   the DrumKit (channel-9 path); keyboard notes map into the
//               pads as pad = (note - 36) mod 8, so any octave plays pads
//   3 "Bass"    dedicated Synth (channel 2), mono-ish bass patch, notes
//               transposed -12 (live routing — same category as the octave
//               shift, which recorded clips also don't bake in)
// Voice budget: the 64-voice pool is PER Synth (three synths + the
// sampler's own 64-voice pool), so 4 tracks never contend for voices.
#pragma once

#include <cstdint>
#include "audio/engine.hpp"
#include "audio/synth.hpp"
#include "audio/drum_kit.hpp"
#include "core/mpmc_queue.hpp"
#include "core/spsc_queue.hpp"
#include "mapping/mapping.hpp"
#include "midi/alsa_client.hpp"
#include "music/arp.hpp"
#include "music/scales.hpp"
#include "music/clip.hpp"
#include "music/recorder.hpp"
#include "music/velocity.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include "project/journal.hpp"

namespace addu94::app {

/// What sound a track index carries (UI + routing share this).
enum class TrackRole : std::uint8_t { SynthA, SynthB, DrumKit, Bass };

/// Track count as a namespace constant: member declarations (std::array
/// bounds) cannot call PlayEngine::track_count() — the class is still
/// incomplete at that point.
inline constexpr int kTrackCount = 4;

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

    // --- track API (app thread; another agent codes UI against these
    //     signatures verbatim) ---
    [[nodiscard]] static constexpr int track_count() noexcept { return kTrackCount; }
    /// "Keys A", "Keys B", "Drums", "Bass" — empty string out of range.
    [[nodiscard]] char const* track_name(int idx) const noexcept;
    [[nodiscard]] TrackRole track_role(int idx) const noexcept;
    /// Selected track index (readable from any thread; relaxed atomic —
    /// the MIDI thread reads it to route notes).
    [[nodiscard]] int selected_track() const noexcept {
        return selected_track_.load(std::memory_order_relaxed);
    }
    /// App thread. Clamps into 0..3.
    void select_track(int idx) noexcept;
    /// 0..1 stored value (UI display). Out of range -> 0.
    [[nodiscard]] float track_volume(int idx) const noexcept {
        return idx >= 0 && idx < track_count()
                   ? track_vol_[static_cast<std::size_t>(idx)].load(
                         std::memory_order_relaxed)
                   : 0.f;
    }
    /// 0..1 stored value (0 = hard left, 0.5 = center, 1 = hard right).
    /// Applied RT as per-track stereo gain. Out of range -> 0.5.
    [[nodiscard]] float track_pan(int idx) const noexcept {
        return idx >= 0 && idx < track_count()
                   ? track_pan_[static_cast<std::size_t>(idx)].load(
                         std::memory_order_relaxed)
                   : 0.5f;
    }
    /// 0..1 stored value. HONEST LIMITATION: stored for the FX-send
    /// milestone — src/audio/fx.hpp's FxChain is an INSERT chain (EQ ->
    /// delay -> chorus) with no shared send bus, so there is nothing
    /// RT-safe to wire it into yet. No audio is faked.
    [[nodiscard]] float track_send(int idx) const noexcept {
        return idx >= 0 && idx < track_count()
                   ? track_send_[static_cast<std::size_t>(idx)].load(
                         std::memory_order_relaxed)
                   : 0.f;
    }

    // --- pad packs (app thread; UI agent codes against these verbatim) ---
    struct PadPackInfo {
        int index;
        std::string name;
    };
    /// The built-in packs, in stable order ("MPC Classic", "808", "Lo-Fi",
    /// "Techno").
    [[nodiscard]] std::vector<PadPackInfo> const& pad_packs() const noexcept;
    [[nodiscard]] int current_pad_pack() const noexcept {
        return drums_.current_pack();
    }
    /// Switch the active pack (control thread; a single atomic store on the
    /// RT side — see DrumKit header). Returns false + `error` on a bad
    /// index or an uninitialized kit.
    bool set_pad_pack(int index, std::string& error);
    /// Wraps around at the end of the pack list.
    void cycle_pad_pack(std::string& error);

    // --- state queries (app thread; relaxed atomics/snapshots) ---
    [[nodiscard]] bool audio_running() const noexcept {
        return audio_.is_running();
    }
    [[nodiscard]] music::Clip& current_clip() noexcept { return clip_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    [[nodiscard]] int octave() const noexcept { return octave_; }
    [[nodiscard]] bool device_connected() const noexcept {
        return device_connected_.load(std::memory_order_acquire);
    }
    /// Scrub position in bars (coarse knob = bars, fine knob = fraction).
    /// HONEST LIMITATION: stored only — music::TransportControl has no
    /// position-set control yet (start/stop/record requests only), so
    /// scrubbing cannot move the playhead without faking it. This value
    /// becomes the transport target the moment that control lands.
    [[nodiscard]] double scrub_position() const noexcept {
        return static_cast<double>(scrub_coarse_) +
               static_cast<double>(scrub_fine_) / 128.0;
    }

    // --- commands (app thread) ---
    void command(mapping::Command const& c);

    // --- master output level (UI meter tap) ---
    // RT writes the peak |sample| of the last rendered buffer (relaxed store);
    // the UI reads it each tick with its own decay. Plain atomics, no locks.
    [[nodiscard]] float master_level() const noexcept {
        return master_level_.load(std::memory_order_relaxed);
    }

    /// Test driver: inject a note through the SAME queue path as hardware
    /// (event_q_ → drain_app_events → recorder/journal). Not used by the
    /// shipped app paths.
    bool test_feed_note(int channel, int note, int velocity, bool on) noexcept {
        midi::MidiEvent ev{};
        ev.type = on ? midi::Type::NoteOn : midi::Type::NoteOff;
        ev.channel = static_cast<std::uint8_t>(channel);
        ev.d1 = static_cast<std::uint8_t>(note);
        ev.d2 = static_cast<std::uint8_t>(velocity);
        ev.stamp_ns = 1;
        return event_q_.push(ev);
    }

    // --- project persistence (spec §6.3/§8) ---
    /// Attach a project dir: loads project.json + replays journal, then
    /// keeps journaling live edits (clip records) with per-append fsync.
    bool attach_project(std::string const& dir, std::string& error);
    /// Full atomic save (journal checkpoint + project.json).
    bool save_project(std::string& error);

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
    void journal_new_events(std::size_t before);  // app thread

    /// Load profile.toml (project dir first, then user config dir) into the
    /// mapping engine; a no-op after the first success. App thread only
    /// (file I/O + TOML parse must never touch the RT path).
    void load_profile_config();

    /// Route one note INTO the selected track's instrument. Wait-free (one
    /// queue push): called from the MIDI thread (live keys) AND the app
    /// thread (arp strikes, chord slots) — hence synth_q_ is an MPMC queue.
    /// `note` is absolute (octave/scale already applied by the caller);
    /// this applies only the per-track specifics (bass -12, drum pad map).
    void push_voice(int track, int note, int velocity, bool on) noexcept;

    /// MIDI channel a track's notes ride (matches the routing table above).
    [[nodiscard]] static constexpr int track_channel(int track) noexcept {
        return track == 0 ? 0 : track == 1 ? 1 : track == 3 ? 2 : 9;
    }

    // RT voice command (trivially copyable, rides the queue). `track` picks
    // the destination instrument; `channel` is the MIDI channel within that
    // instrument (voice identity for note_off).
    struct VoiceCmd {
        enum class Kind : std::uint8_t { Note, PedalOff };
        Kind kind{Kind::Note};
        std::uint8_t track{0};
        std::uint8_t note{0};
        std::uint8_t channel{0};
        std::uint8_t velocity{0};
        bool on{true};
    };
    /// RT parameter update (trivially copyable): a whole SynthParams for one
    /// track — knob turns are infrequent, a full-struct message is simpler
    /// and race-free compared to per-field atomics.
    struct ParamCmd {
        std::uint8_t track{0};
        audio::SynthParams params{};
    };

    audio::AudioEngine audio_;
    midi::AlsaClient midi_;
    mapping::MappingEngine mapping_;
    // One synth per track; index 2 (Drums) is unused but constructed so the
    // track index maps directly (an optional/array-of-pointers would buy
    // branchy code for one Synth's worth of voice pool — not worth it).
    std::array<audio::Synth, kTrackCount> synths_{};
    audio::DrumKit drums_;
    music::Recorder recorder_;
    music::Clip clip_;

    static constexpr std::size_t kQueueCap = 1024;
    // MPMC (not SPSC) on purpose: live keys push from the MIDI thread while
    // arp strikes and chord slots push from the app thread — that is two
    // producers, which SPSC cannot guarantee. core::MpmcQueue is lock-free
    // and RT-safe (TSan-covered, already used by the logger).
    core::MpmcQueue<VoiceCmd, kQueueCap> synth_q_;    // MIDI/app → audio
    core::SpscQueue<ParamCmd, 64> param_q_;           // app → audio
    core::SpscQueue<midi::MidiEvent, kQueueCap> event_q_;  // MIDI → app

    music::VelocityCurve vel_curve_{music::VelocityCurve::Log};
    int octave_{0};
    std::atomic<int> selected_track_{0};  // MIDI thread reads, app writes
    std::array<std::atomic<float>, kTrackCount> track_vol_{};  // 0..1
    std::array<std::atomic<float>, kTrackCount> track_pan_{};  // 0..1
    std::array<std::atomic<float>, kTrackCount> track_send_{}; // stored only
    std::array<audio::SynthParams, kTrackCount> synth_params_{};  // app truth
    std::atomic<float> master_gain_{1.0f};  // clamp 0..1.5, applied in render
    bool recording_{false};
    std::atomic<bool> sustain_pedal_{false};
    std::atomic<bool> device_connected_{false};

    // Arp: held keys bypass synth when arp is on; arp strikes go via
    // synth_q_ on grid crossings (app thread → RT queue).
    project::Journal journal_;
    std::string project_dir_;
    bool journal_open_{false};
    bool profile_loaded_{false};  // attach_project() wins over start()

    music::Arp arp_;
    std::atomic<bool> arp_on_{false};

    // Chord memory (spec §5): 6 slots on bank-B pads 46..51.
    // press: sound the stored chord (note-ons), unless empty; hold >= 400 ms
    // with keys held: capture current keys into the slot on release. Distinct
    // musical response per pad — the fix for the "pads all same" complaint.
    static constexpr int kChordSlots = 6;
    std::array<std::vector<int>, kChordSlots> chord_slots_{};
    std::array<std::chrono::steady_clock::time_point, kChordSlots>
        chord_press_{};
    std::vector<int> chord_sounding_;   // notes currently held by a slot
    int chord_sounding_track_{-1};      // track the sounding chord targeted
    std::vector<int> chord_held_keys_;  // keys held right now (absolute)
    std::atomic<float> master_level_{0.0f};  // RT: peak |sample| per buffer
    std::atomic<bool> scale_lock_{false};
    music::Scale const* scale_{&music::kScales[0]};  // chromatic default
    std::uint8_t scale_root_{0};
    std::int64_t last_arp_grid_{-1};
    std::vector<int> arp_held_keys_;  // raw keys (pre-octave) while arp on
    int scrub_coarse_{0};  // stored (see scrub_position) until a transport
    int scrub_fine_{0};    // position-set control exists

    // Per-render scratch (pre-allocated: the RT path renders each track into
    // scratch, applies its gain/pan, then adds into the engine buffers —
    // Synth has overwrite semantics, the mix needs accumulation).
    static constexpr std::uint32_t kMaxBlock = 8192;
    std::array<float, kMaxBlock> scratch_l_{};
    std::array<float, kMaxBlock> scratch_r_{};
};

}  // namespace addu94::app
