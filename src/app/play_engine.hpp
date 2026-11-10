// ghost_story — src/app/play_engine.hpp
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
#include "audio/fx.hpp"
#include "audio/drum_kit.hpp"
#include "audio/capture_ring.hpp"
#include "core/mpmc_queue.hpp"
#include "core/spsc_queue.hpp"
#include "mapping/mapping.hpp"
#include "midi/alsa_client.hpp"
#include "music/arp.hpp"
#include "music/scales.hpp"
#include "music/clip.hpp"
#include "music/clip_player.hpp"
#include "music/recorder.hpp"
#include "music/retrigger.hpp"
#include "music/velocity.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "project/journal.hpp"

namespace ghost_story::app {

/// What sound a track carries (UI + routing share this). Tracks are
/// DYNAMIC now: the user adds them from the + instrument menu and removes
/// them at will (user report: "it's not necessary to have keys a keys b
/// drum and bass — I will add them manually as needed").
enum class TrackKind : std::uint8_t { Synth, Drums };

/// Slot-pool size as a namespace constant: member declarations (std::array
/// bounds) cannot call PlayEngine::track_count() — the class is still
/// incomplete at that point. track_count() returns the ACTIVE count; the
/// app starts with zero tracks and slots activate from the bottom.
/// 8 slots map 1:1 onto the EDIT-mode bank-A pads (TrackSelect 0..7).
inline constexpr int kTrackCount = 8;

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
    /// ACTIVE track count (the pool is kTrackCount slots; the app starts
    /// with zero tracks and the user adds them from the + menu).
    [[nodiscard]] int track_count() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }
    /// The slot's name (preset name / "Drums") — empty string out of range.
    [[nodiscard]] char const* track_name(int idx) const noexcept;
    /// The slot's sound kind — Synth out of range.
    [[nodiscard]] TrackKind track_kind(int idx) const noexcept;
    /// Add a track from a preset template (index into presets(); pass -1
    /// for the Drum Kit). App thread. Returns false + `error` when the
    /// pool is full or a Drums track already exists (the kit is shared).
    bool add_track(int preset_index, std::string& error);
    /// Remove track `idx` (flushes pending captures, shifts later slots
    /// down, clamps the selection, journals the removal). App thread.
    bool remove_track(int idx, std::string& error);
    /// True once a Drums track exists (the kit is shared — the + menu
    /// disables a second one).
    [[nodiscard]] bool drums_present() const noexcept {
        return drums_slot() >= 0;
    }
    /// Selected track index (readable from any thread; relaxed atomic —
    /// the MIDI thread reads it to route notes).
    [[nodiscard]] int selected_track() const noexcept {
        return selected_track_.load(std::memory_order_relaxed);
    }
    /// App thread. Clamps into 0..3.
    void select_track(int idx) noexcept;
    /// Mode switch (app thread): the mapping's mode plus the relaxed
    /// atomic the MIDI thread gates the pad-drum path on. ALL mode
    /// switches must go through this — the direct
    /// mapping().set_mode(...) route would leave mode_rt_ stale and the
    /// RT gate firing drums in EDIT/KIT.
    void set_mode(mapping::Mode m) noexcept;
    /// The mode the MIDI thread sees (0 Play, 1 Edit, 2 Step). Only
    /// set_mode() writes it.
    [[nodiscard]] mapping::Mode mode() const noexcept {
        return static_cast<mapping::Mode>(
            mode_rt_.load(std::memory_order_relaxed));
    }
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
    /// 0..1 stored value — the track's reverb amount. The TrackSend knob
    /// writes it into the track's FxParams (reverb_mix + on flag) and the
    /// RT path processes the per-track insert reverb; this atomic mirrors
    /// the value for cheap UI display. Out of range -> 0.
    [[nodiscard]] float track_send(int idx) const noexcept {
        return idx >= 0 && idx < track_count()
                   ? track_send_[static_cast<std::size_t>(idx)].load(
                         std::memory_order_relaxed)
                   : 0.f;
    }

    // --- per-index mixer setters (app thread; TrackStrips UI) ---
    // The TrackVolume/TrackPan COMMANDS act on the SELECTED track (the
    // hardware knob's target). These address an explicit row, which is what
    // a four-strip mixer needs; same atomics, same RT application at the
    // next buffer boundary. Out-of-range idx: no-op.
    void set_track_volume(int idx, float v) noexcept;
    void set_track_pan(int idx, float v) noexcept;
    /// Mute gate: the RT mix loop skips a muted track's contribution (its
    /// FX chain still processes so delay/reverb tails keep decaying).
    void set_track_mute(int idx, bool on) noexcept;
    [[nodiscard]] bool track_mute(int idx) const noexcept {
        return idx >= 0 && idx < track_count() &&
               track_mute_[static_cast<std::size_t>(idx)].load(
                   std::memory_order_relaxed);
    }
    /// Record arming (bit `idx` of the mask = track `idx`). When any bit is
    /// set, the loop recorder takes notes ONLY from armed tracks; with no
    /// bits set, recording stays the shipped everything-live behavior.
    void set_record_arm(int idx, bool on) noexcept;
    [[nodiscard]] unsigned record_arm_mask() const noexcept {
        return record_arm_.load(std::memory_order_relaxed);
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

    // --- chord slots (app thread; UI mirror filled-slot indicator) ---
    /// Number of notes captured in bank-B chord slot 0..5; 0 = empty.
    /// (An empty slot press is silent — the mirror shows captured slots as
    /// filled so "nothing happens" is never a mystery.)
    [[nodiscard]] int chord_slot_count(int slot) const noexcept {
        return slot >= 0 && slot < kChordSlots
                   ? static_cast<int>(
                         chord_slots_[static_cast<std::size_t>(slot)].size())
                   : 0;
    }

    // --- per-track FX (app thread; hardware FX knobs + FxPanel UI) ---
    // The 9 chain blocks in signal order (fx.hpp: EQ -> comp -> crush ->
    // drive -> chorus -> flanger -> delay -> tremolo -> reverb). The FX
    // SELECT control picks one; TOGGLE flips its on/off; PARAM A/B are the
    // slot's two musical knobs (see fx_slot_param_name).
    enum class FxSlot : int {
        Eq = 0, Comp, Crush, Drive, Chorus, Flanger, Delay, Tremolo, Reverb
    };
    static constexpr int kFxSlotCount = 9;
    /// Short UI name ("EQ", "Comp", "Crush", "Drive", "Chorus", "Flanger",
    /// "Delay", "Trem", "Verb"); nullptr out of range.
    [[nodiscard]] static char const* fx_slot_name(int slot) noexcept;
    /// Label for the slot's A (ab == 0) or B (ab == 1) parameter knob;
    /// nullptr out of range.
    [[nodiscard]] static char const* fx_slot_param_name(int slot,
                                                        int ab) noexcept;
    /// One-line plain-language definition of the slot ("what it does to the
    /// sound"), for the FX panel's description strip; nullptr out of range.
    [[nodiscard]] static char const* fx_slot_desc(int slot) noexcept;
    /// Copy snapshot of the track's FX params (app thread; trivially
    /// copyable). Out of range -> default (all bypass).
    [[nodiscard]] audio::FxParams track_fx_params(int track) const noexcept {
        return track >= 0 && track < track_count()
                   ? fx_params_[static_cast<std::size_t>(track)]
                   : audio::FxParams{};
    }
    /// App thread. Whole-struct update (mirrors the synth ParamCmd
    /// philosophy): stored as app truth AND pushed to the RT chain, applied
    /// at the next buffer boundary. Also syncs the track_send_ mirror
    /// atomic from reverb_mix. Out-of-range track: no-op.
    void set_track_fx_params(int track, audio::FxParams const& p);
    /// Currently selected FX slot (hardware knob + panel focus); clamped.
    [[nodiscard]] int fx_slot() const noexcept { return fx_slot_; }
    void set_fx_slot(int slot) noexcept {
        fx_slot_ = slot < 0 ? 0 : (slot >= kFxSlotCount ? kFxSlotCount - 1
                                                        : slot);
    }

    // --- synth param access (app thread; UI display of the patch) ---
    /// Copy snapshot of the track's synth patch. Out of range -> default.
    [[nodiscard]] audio::SynthParams track_synth_params(int track) const
        noexcept {
        return track >= 0 && track < track_count()
                   ? synth_params_[static_cast<std::size_t>(track)]
                   : audio::SynthParams{};
    }

    // --- presets (app thread; FxPanel chips) ---
    struct PresetInfo {
        int index;
        std::string name;
    };
    /// The curated {synth patch + FX chain} presets, in stable order. Each
    /// exercises different blocks of the chain. Applying to a track sets
    /// synth_params_ + fx_params_ (pushed to RT), syncs the send knob
    /// mirror, and marks the track as "on preset N"; any manual knob edit
    /// marks it custom (-1).
    [[nodiscard]] std::vector<PresetInfo> const& presets() const noexcept;
    /// -1 = custom/unknown (knob edits since the last apply). Out of range
    /// -> -1.
    [[nodiscard]] int current_preset(int track) const noexcept {
        return track >= 0 && track < track_count()
                   ? track_preset_[static_cast<std::size_t>(track)].load(
                         std::memory_order_relaxed)
                   : -1;
    }
    /// Apply preset `index` to `track` (Drums track: no-op, false — packs
    /// are the drum presets). Returns false + `error` on bad indices.
    bool apply_preset(int index, int track, std::string& error);

    // --- state queries (app thread; relaxed atomics/snapshots) ---
    [[nodiscard]] bool audio_running() const noexcept {
        return audio_.is_running();
    }
    /// The track's working clip (loop machine: one clip per track; keys
    /// record into the SELECTED track's clip, pads into the Drums clip).
    [[nodiscard]] music::Clip& clip(int track) noexcept {
        return clips_[static_cast<std::size_t>(track)];
    }
    [[nodiscard]] music::Clip const& clip(int track) const noexcept {
        return clips_[static_cast<std::size_t>(track)];
    }
    /// Clip length in whole bars (meter-aware; ≥ 1 when a clip exists).
    /// NB: non-const — TransportControl access has no const overload.
    [[nodiscard]] int clip_bars(int track) noexcept;
    /// Clip placement in bars (its start_tick converted; 0 = top).
    [[nodiscard]] int clip_start_bar(int track) noexcept;
    /// True when the track's clip holds at least one event.
    [[nodiscard]] bool clip_has_notes(int track) const noexcept {
        return track >= 0 && track < track_count() &&
               !clip(track).events.empty();
    }
    /// App thread: resize a track's clip to `bars` (≥ 1). Used by the
    /// arrangement canvas (double-click on an empty lane = reserve — this
    /// is how a second instrument joins the loop).
    void set_clip_bars(int track, int bars);
    /// App thread: move the playhead to `bar` (arrangement click). Also
    /// becomes the start position for the next Play / first-note record —
    /// "record from specific times".
    void set_playhead_bar(int bar);
    /// App thread: place the track's clip at `bar` (≥ 0) — drag-to-move in
    /// the arrangement; journaled (ClipMove) and republished. The whole
    /// take-group moves: window AND its take boxes shift together (notes
    /// are clip-local, so they ride the window).
    void set_clip_start(int track, int bar);
    /// App thread: resize ONE take's box to `bars` (≥ 1) — drag the edge
    /// of a specific [rec n] block. That take's events past the new end
    /// are dropped with it (the box is the take).
    void set_box_bars(int track, int take, int bars);
    /// App thread: wipe ONE take's notes + box (right-click a block →
    /// Clear this recording). The track and its other takes survive.
    void clear_take(int track, int take);
    /// App thread: wipe the track's recorded notes (right-click → Clear).
    /// Placement (start_tick) is kept; journaled (ClipClear).
    void clear_clip(int track);
    /// One take box as the canvas draws it (bars; has_notes = any event
    /// inside the box's span).
    struct ClipBoxView {
        int take;
        int start_bar;
        int bars;
        bool has_notes;
        friend bool operator==(ClipBoxView const& a,
                               ClipBoxView const& b) noexcept {
            return a.take == b.take && a.start_bar == b.start_bar &&
                   a.bars == b.bars && a.has_notes == b.has_notes;
        }
    };
    /// The track's recorded takes as blocks ([rec 1] [rec 2] …). Empty
    /// while nothing was recorded — the canvas then draws the reserved
    /// window (clip_start_bar/clip_bars) as a dashed block instead.
    /// NB: non-const — TransportControl access has no const overload.
    [[nodiscard]] std::vector<ClipBoxView> clip_boxes(int track);
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    /// Take placement (take_cover): bit t set = track t's clip is live in
    /// the current take — its block starts at the take's bar and grows with
    /// the playhead, so the canvas shows the take being placed as you play.
    [[nodiscard]] unsigned recording_mask() const noexcept {
        return take_mask_.load(std::memory_order_relaxed);
    }
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
    /// Master gain 0..1.5 (unity 1.0) — the MasterVolume knob's stored
    /// value, read by the HealthStrip master fader for display.
    [[nodiscard]] float master_gain() const noexcept {
        return master_gain_.load(std::memory_order_relaxed);
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
    /// Test driver: run the REAL audio callback `count` times (frames each)
    /// synchronously on the calling thread — transport advances, clips
    /// schedule, the mix renders. Returns the peak |sample| seen. Not used
    /// by shipped app paths (tests + selftest-record's playback assertion
    /// use it).
    float test_render_audio(std::uint32_t frames, std::uint32_t count) noexcept;
    /// Test driver: the start()-without-backends subset — FX chains +
    /// synths + drum kit at 48 kHz. Without it the mix renders silence
    /// (uninitialized FX rings) and the kit has no zones; the real app
    /// path gets all of this from start().
    void test_init_audio() noexcept;

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

    // --- velocity curve (app thread setter; the MIDI thread applies it
    //     to every note-on) ---
    [[nodiscard]] music::VelocityCurve velocity_curve() const noexcept {
        return vel_curve_.load(std::memory_order_relaxed);
    }
    void set_velocity_curve(music::VelocityCurve c) noexcept {
        vel_curve_.store(c, std::memory_order_relaxed);
    }

    // --- mic / live input (backend capture -> RT mix -> grab-to-pad) ---
    // The backend pushes captured frames into mic_ring_; render_into pops
    // them, rolls a post-gain history ring, and (when monitoring) mixes the
    // mic into the master BEFORE the master fader — so the mic follows the
    // MasterVolume knob like every other source. Monitoring defaults OFF:
    // open mics through speakers feed back (howl/scratch), and every DAW
    // ships input monitoring opt-in for exactly that reason. The MicPanel
    // toggle turns it on when wanted (headphones recommended). Grab-to-pad
    // works regardless — it reads the history ring, not the monitor mix.
    void set_mic_gain(float g) noexcept;  // clamped 0..2
    [[nodiscard]] float mic_gain() const noexcept {
        return mic_gain_.load(std::memory_order_relaxed);
    }
    void set_mic_monitor(bool on) noexcept {
        mic_monitor_.store(on, std::memory_order_relaxed);
    }
    [[nodiscard]] bool mic_monitor() const noexcept {
        return mic_monitor_.load(std::memory_order_relaxed);
    }
    /// RT peak |sample| of the last mic buffer (post-gain), UI meter tap.
    [[nodiscard]] float mic_level() const noexcept {
        return mic_level_.load(std::memory_order_relaxed);
    }
    /// True while the capture path is actually delivering frames (the RT
    /// callback stamps arrivals; the UI ages the stamp out when input
    /// goes silent or no backend could open a capture stream).
    [[nodiscard]] bool mic_active() const noexcept;
    /// Grab the last `seconds` of captured mic audio as the new zone for
    /// drum pad `pad` (0..7 = bank A, note 36+pad): mono downmix, peak
    /// normalized to 0.9, loaded via DrumKit::load_override_data. App
    /// thread; returns false + `error` when the pad is out of range or
    /// not enough audio has been captured yet.
    bool grab_mic_to_pad(int pad, double seconds, std::string& error);
    /// Test hook: push frames into the mic ring exactly like a backend.
    bool test_feed_mic(float const* l, float const* r,
                       std::uint32_t n) noexcept;

    // --- keys play-once guard (user report: "played multiple times on
    // pressing" — the MPK's onboard arpeggiator re-fires held keys) ---
    // When on, a NoteOn for an already-held key (same channel+note, no
    // NoteOff between) is dropped before it reaches a voice. Default on: a
    // keybed press should sound exactly once; toggle REPEAT to pass
    // hardware-arp repeat pulses through.
    [[nodiscard]] bool keys_play_once() const noexcept {
        return keys_play_once_.load(std::memory_order_acquire);
    }
    void set_keys_play_once(bool on) noexcept {
        keys_play_once_.store(on, std::memory_order_release);
        if (!on) retrigger_.clear();
    }

private:
    static void render_trampoline(float* l, float* r, std::uint32_t f,
                                  void* user) noexcept;
    void process_arp() noexcept;  // app thread: strike grid crossings
    void journal_new_events(int track, std::size_t before);  // app thread
    /// App thread: push every track's clip into its RT ClipPlayer snapshot
    /// (called from drain_app_events when a clip's shape changed, or the
    /// tempo/sample rate moved — the players' offsets are precomputed).
    void republish_clips() noexcept;
    /// Real output sample rate (audio_.stats()), sanity-clamped. The old
    /// tick math hardcoded 48000 — wrong tempo conversion on other rates.
    [[nodiscard]] double app_sample_rate() noexcept;
    /// Apply a voice straight to the instruments — RT thread only (both
    /// the synth_q_ drain and the clip players' fires land here).
    void apply_voice(int track, int channel, int note, int velocity,
                     bool on) noexcept;

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
    /// RT FX update: a whole FxParams for one track — same whole-struct
    /// philosophy (FxChain::set_params is alloc-free pure math, applied
    /// inside the audio callback at the next buffer boundary).
    struct FxParamCmd {
        std::uint8_t track{0};
        audio::FxParams params{};
    };

    audio::AudioEngine audio_;
    midi::AlsaClient midi_;
    mapping::MappingEngine mapping_;
    // One synth per track; index 2 (Drums) is unused but constructed so the
    // track index maps directly (an optional/array-of-pointers would buy
    // branchy code for one Synth's worth of voice pool — not worth it).
    std::array<audio::Synth, kTrackCount> synths_{};
    // One FX chain per track (drums included — glue EQ/delay on the kit).
    // Rings are allocated once in start() via set_sample_rate (pre-RT);
    // everything afterwards is alloc-free.
    std::array<audio::FxChain, kTrackCount> fx_chains_{};
    audio::DrumKit drums_;
    music::Recorder recorder_;
    // Dynamic tracks: a fixed pool of slots, activated from the bottom.
    // The kind/transpose are relaxed ATOMICS because the RT mix loop and
    // the MIDI thread read them while the app thread rewrites slots on
    // add/remove; `name` is app-thread only. clips_ MUST stay fixed
    // contiguous storage — Recorder pendings hold raw Clip* into it.
    struct TrackSlot {
        std::atomic<std::uint8_t> kind{static_cast<std::uint8_t>(
            TrackKind::Synth)};
        std::atomic<int> transpose{0};  // semitones (legacy Bass: -12)
        std::string name{"Track"};
    };
    std::array<TrackSlot, kTrackCount> slots_{};
    std::atomic<int> active_{0};  // slots 0..active-1 are live
    /// The active Drums slot (shared kit), or -1 when none exists.
    [[nodiscard]] int drums_slot() const noexcept;
    /// Record-arm gate: channel -> owning slot (synth slots own channel ==
    /// slot index, the drums slot owns channel 9). Returns the slot when
    /// the arm mask lets the channel record, else -1.
    [[nodiscard]] int channel_track(unsigned arm_mask, int channel) const noexcept;
    // Loop machine: one working clip per track. Keys record into the
    // SELECTED track's clip (pads always into the Drums clip); every clip
    // plays back against the transport via its ClipPlayer snapshot.
    std::array<music::Clip, kTrackCount> clips_{};
    std::array<music::ClipPlayer, kTrackCount> clip_players_{};
    // Clip-shape bookkeeping for republish_clips(): (events.size(),
    // length_ticks, start_tick) per track — all clip mutations insert,
    // erase or re-place, so this triple catches every change without a
    // revision counter.
    std::array<std::tuple<std::size_t, std::int64_t, std::int64_t>,
               kTrackCount>
        published_shape_{};
    double published_bpm_{0.0};   // 0 = never published
    double published_sr_{0.0};
    std::uint8_t last_play_state_{0};  // RT-owned: previous buffer's state
    // Record-side twin of the MIDI-thread play-once guard: on_midi pushes
    // EVERY event to event_q_ (sound-suppressed re-fires included), so the
    // recorder needs the same suppression or a held key records doubles.
    music::RetriggerGuard record_retrigger_;

    static constexpr std::size_t kQueueCap = 1024;
    // MPMC (not SPSC) on purpose: live keys push from the MIDI thread while
    // arp strikes and chord slots push from the app thread — that is two
    // producers, which SPSC cannot guarantee. core::MpmcQueue is lock-free
    // and RT-safe (TSan-covered, already used by the logger).
    core::MpmcQueue<VoiceCmd, kQueueCap> synth_q_;    // MIDI/app → audio
    core::SpscQueue<ParamCmd, 64> param_q_;           // app → audio
    core::SpscQueue<FxParamCmd, 64> fx_q_;            // app → audio
    core::SpscQueue<midi::MidiEvent, kQueueCap> event_q_;  // MIDI → app
    // KIT knobs push a whole SamplerParams; the RT path applies it at the
    // buffer boundary (same whole-struct philosophy as ParamCmd — a struct
    // assignment mid-note_on on the control thread would race the read).
    core::SpscQueue<audio::SamplerParams, 8> kit_q_;  // app → audio

    // Velocity curve: written by the app thread (HealthStrip combo / KIT
    // knob), read by the MIDI thread per note-on — hence atomic.
    std::atomic<music::VelocityCurve> vel_curve_{music::VelocityCurve::Log};
    std::atomic<unsigned> record_arm_{0};  // bit t = track t record-armed
    int octave_{0};
    std::atomic<int> selected_track_{0};  // MIDI thread reads, app writes
    std::atomic<int> mode_rt_{0};  // 0 Play 1 Edit 2 Step — MIDI thread's
                                   // pad-drum gate (writes: set_mode only)
    std::array<std::atomic<float>, kTrackCount> track_vol_{};  // 0..1
    std::array<std::atomic<float>, kTrackCount> track_pan_{};  // 0..1
    std::array<std::atomic<float>, kTrackCount> track_send_{}; // reverb mirror
    std::array<std::atomic<bool>, kTrackCount> track_mute_{};  // RT mix gate
    audio::SamplerParams kit_params_{};  // app truth for the KIT knobs
    int last_drum_pad_{0};               // KIT tune/decay target (0..7)
    std::array<audio::SynthParams, kTrackCount> synth_params_{};  // app truth
    std::array<audio::FxParams, kTrackCount> fx_params_{};        // app truth
    int fx_slot_{0};                    // selected FX slot (app thread)
    double fx_tempo_{0.0};              // last bpm pushed to the chains (RT)
    std::array<std::atomic<int>, kTrackCount> track_preset_{};  // -1 = custom
    // Legacy patch structs: byte-identical sounds for projects saved by the
    // old fixed-4 engine (see attach_project's legacy adopt).
    audio::SynthParams legacy_keys_b_{};
    audio::SynthParams legacy_bass_{};
    std::string last_kit_error_;  // kit init diagnostics (test + start path)
    std::atomic<float> master_gain_{1.0f};  // clamp 0..1.5, applied in render
    bool recording_{false};
    // Take placement (app thread): while recording, capturing clips are
    // re-placed to the take's bar-floored start tick and grown to the
    // playhead — the block on the canvas IS the take. take_mask_ is read
    // by the UI (recording_mask) to draw the live REC glow.
    std::atomic<unsigned> take_mask_{0};
    std::int64_t take_start_tick_{-1};
    void take_cover(int track, std::int64_t now);  // app thread
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

    // Keys play-once guard: owned by the MIDI thread (touched only in
    // on_midi, wait-free by contract — see music/retrigger.hpp).
    music::RetriggerGuard retrigger_;
    std::atomic<bool> keys_play_once_{true};

    // Chord memory (spec §5): 6 slots on bank-B pads 46..51.
    // press: sound the stored chord (note-ons), unless empty; hold >= 400 ms
    // with keys played during the hold: capture into the slot on release.
    // Distinct musical response per pad — the fix for the "pads all same"
    // complaint. The capture keys are snapshotted the moment any key is down
    // during the hold (chord_capture_), so releasing the keys before the pad
    // — the natural gesture order — still captures (user report: "BANK B
    // doesn't work at all": empty slots were silent, and the old rule only
    // captured keys still held at pad release).
    static constexpr int kChordSlots = 6;
    std::array<std::vector<int>, kChordSlots> chord_slots_{};
    std::array<std::chrono::steady_clock::time_point, kChordSlots>
        chord_press_{};
    std::array<bool, kChordSlots> chord_holding_{};     // press seen, release
                                                        // not yet processed
    std::array<std::vector<int>, kChordSlots> chord_capture_{};  // snapshot
    std::vector<int> chord_sounding_;   // notes currently held by a slot
    int chord_sounding_track_{-1};      // track the sounding chord targeted
    std::vector<int> chord_held_keys_;  // keys held right now (absolute)
    int last_loop_bars_{4};             // LoopToggle's restore length
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

    // Live input (see the mic API above). Ring capacities are power-of-two
    // frame counts; both rings are heap-backed control-thread allocations
    // whose push/pop are RT-safe (audio/capture_ring.hpp).
    static constexpr std::uint32_t kMicRingFrames = 16384;   // ~340 ms @48k
    audio::CaptureRing mic_ring_{kMicRingFrames};
    std::unique_ptr<audio::HistoryRing> mic_history_;        // ~10.9 s @48k
    std::array<float, kMaxBlock> mic_l_{};
    std::array<float, kMaxBlock> mic_r_{};
    std::atomic<float> mic_gain_{1.0f};    // 0..2, applied before history
    std::atomic<bool> mic_monitor_{false};  // opt-in: feedback through speakers
    std::atomic<float> mic_level_{0.0f};   // RT peak tap for the UI meter
    std::atomic<std::int64_t> mic_last_ms_{0};  // RT stamps frame arrivals
};

}  // namespace ghost_story::app
