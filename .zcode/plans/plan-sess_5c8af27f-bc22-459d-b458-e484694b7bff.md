# addu94: useful knobs/buttons + live mic input

## Context

The app is a C++26/Qt6 groovebox ("one screen = the instrument"). Two problems:

1. **Many on-screen controls are dead or display-only.** TrackStrips volume/pan/mute/arm mutate widget-local arrays and only log (`src/ui/track_strip.hpp:13-15`, hook at `main_window.cpp:535`); ControllerMirror's 8 knobs and 16 pads can't be clicked/dragged (`controller_mirror.cpp:530-576` handles only chips); the FX panel tooltip promises click-to-toggle but doesn't implement it; the HealthStrip velocity combo is a no-op (`health_strip.cpp:56-61`); there is no master-volume UI; the UI stop button bypasses the recorder-flush/project-save that hardware stop does (`main_window.cpp:476-479` vs `play_engine.cpp:1065-1075`); KIT-mode knobs 1–8 are all `Action::None` (`mapping.cpp:150-153`).
2. **There is no audio input path at all** — backends are output-only (`RenderFn` contract, `backend.hpp:41`), while `Sampler::add_zone_data()` (`sampler.hpp:67`) is a ready-made RT-safe sink for captured PCM.

Goal: make every visible control actually drive the engine (usable live without the MPK), and add a mic channel you can monitor live over the beat and **grab into a drum pad** (sample your voice/percussion into the kit while playing).

---

## Part A — Make the knobs and buttons real

### A1. Interactive ControllerMirror (knobs + pads)
- `mapping.hpp/.cpp`: hoist the per-mode knob tables out of `map_cc` into a shared `static Action knob_action(Mode, int idx)` plus `knob_label(Mode, int)` (kills the "KEEP IN SYNC" duplication with `controller_mirror.cpp:36-45`). Add `set_knob_value(idx, v)` so UI-driven turns update the display (hardware path updates it inside `map_cc`).
- `controller_mirror.cpp`: cache `knob_rects_[8]` in `paint_knobs`; mouse-drag vertical (±1 value/1.5px, like the BPM block), wheel = ±1, double-click = reset to 64; each move emits `engine_.command({knob_action(mode, i), value, 0})`. EDIT knob 2 (`FxToggle`) is click-to-toggle, not a drag. Pads become press/release-aware: PLAY → `DrumPad` (+ local `pulse_pad` since UI commands don't ride `drain_app_events`); EDIT bank A → `TrackSelect`, B1/B2 → undo/redo, B3–B8 → `ChordSlot` press/release pairs (mouse-down/mouse-up mirrors the hold gesture); KIT → `PadPackSelect`. Update the tooltip.

### A2. TrackStrips drive the real mixer
- `play_engine.hpp/.cpp`: add public per-index setters `set_track_volume(idx, 0..1)`, `set_track_pan(idx, 0..1)` (the existing `TrackVolume` command acts on the *selected* track only — wrong for a 4-row strip) and a mute state the RT mix loop respects: `std::array<std::atomic<bool>, kTrackCount> track_mute_` (skipped in the mix loop at `play_engine.cpp:802-837`) + `set_track_mute/track_mute`. Add an arm bitmask `std::atomic<unsigned> record_arm_`; in `drain_app_events` (recorder feed at `play_engine.cpp:679-691`) gate recording: if any track is armed, only armed tracks' notes are recorded; if none, current behavior.
- `track_strip.cpp/.hpp`: engine becomes the source of truth — drag volume/pan writes the setters (volume range becomes 0..100% to match the engine's 0..1 gain instead of the fictional 150%), mute/arm write engine state, `refresh()` reads engine values each tick. Keep the hook for logging.

### A3. FxPanel slot toggle
- Clicking a chip's ON-dot corner (or right-click on a chip) selects that slot then emits `engine_.command({Action::FxToggle, 0, 0})` — the action already exists (`play_engine.cpp:1192`), the UI just never sent it. Fix the tooltip.

### A4. HealthStrip: velocity curve + master volume
- `play_engine`: make `vel_curve_` `std::atomic<music::VelocityCurve>` with `set_velocity_curve()/velocity_curve()` (it's applied on the MIDI thread at `play_engine.cpp:520,545`).
- `health_strip.cpp`: wire the combo to `set_velocity_curve` and sync its index in `refresh()` (block signals while syncing). Add a compact master-volume slider next to the meter emitting `command({Action::MasterVolume, param})` (0..127 → 0..1.5, unity ≈ 85, `play_engine.cpp:1150-1157`).

### A5. TransportBar consistency
- Stop button (and `shortcut_play` when stopping) route through `engine_.command({Action::TransportStop, 0, 0})` so stopping from the UI also flushes recorder pending notes and saves the project, like hardware stop. Play routes through `TransportPlay` the same way.

### A6. KIT-mode knobs get real bindings
- New actions in `actions.hpp` + `to_string`: `VelocityCurveSet`, `KitGain`, `PadTune`, `PadDecay`.
- KIT knob map (`mapping.cpp` Step branch + `audit.cpp` `step_k[8]`): 1 = VelocityCurveSet (4 zones), 2 = KitGain (sampler gain 0..2), 3 = PadTune (±12 st), 4 = PadDecay (0.1..4 scale), 8 = MasterVolume, rest stay `None`.
- `play_engine::command` cases: KitGain/PadTune/PadDecay build a `SamplerParams` and push via `drums_.set_params` (all packs) / `drums_.sampler().set_params` for pad-targeted ones; tune/decay target `last_drum_pad_` (0..7, updated on every `DrumPad` command and pack audition at `play_engine.cpp:1297`). Mirror KIT labels become `curve, gain, tune, decay, -, -, -, master`.

---

## Part B — Live mic input (monitor + grab-to-pad)

### B1. Lock-free capture rings (new `src/audio/capture_ring.hpp`)
Two small SPSC rings in the house style of `core/spsc_queue.hpp` (power-of-two, cache-line-separated atomics, no alloc/lock in push/pop):
- `CaptureRing` — backend→RT, drop-new-when-full, `push(l, r, n)` / `pop(l, r, max_n)`; capacity 16384 frames (~340 ms).
- `HistoryRing` — RT→app, **overwrite-oldest** on push; `read_last(l, r, n)` compacts the newest n frames out; capacity 2^19 frames (~10.9 s @48k, ~4 MB, heap-allocated via unique_ptr).

### B2. Backend capture (output stays untouched; capture is additive)
- `backend.hpp`: `BackendConfig` gains `CaptureRing* capture_ring{nullptr}` (null = no capture).
- **JACK** (`jack_backend.cpp`): when a ring is provided, register `in_L`/`in_R` `JackPortIsInput` ports in `open()`, push frames in `handle_process`, best-effort auto-connect to `system:capture_1/2` after activate (same pattern as `connect_playback_ports`, `jack_backend.cpp:275`).
- **PipeWire** (`pipewire_backend.cpp`): second `pw_stream` with `PW_DIRECTION_INPUT` on the same `pw_thread_loop` (keeps the SPSC single-producer guarantee), F32P stereo, auto-connect to the default source, push in its process callback.
- **ALSA** (`alsa_backend.cpp`): optional capture thread (`snd_pcm_open` capture, `"default"`, `snd_pcm_readi` loop → float convert → push); failure logs a warning and the app continues playback-only.
- `AudioEngine` passes the config through unchanged; `PlayEngine` supplies the ring before `audio_.start()`.

### B3. PlayEngine mic channel
- Members: `mic_ring_`, `mic_history_`, atomics `mic_gain_` (0..2, default 1), `mic_monitor_` (default **on** — tooltip warns about speaker feedback), `mic_level_` (RT peak for the UI meter), `mic_active_` + `mic_last_ms_` (RT stamps whenever frames arrive; UI ages it out).
- `render_into` (`play_engine.cpp:739`): pop from `mic_ring_` into new pre-allocated `mic_l_/mic_r_` scratch (kMaxBlock), apply gain, peak→`mic_level_`, push post-gain into `mic_history_`; if monitoring, accumulate into the master buffer *before* master gain. Always pop (grab works even when muted).
- App APIs: `set_mic_gain/mic_gain`, `set_mic_monitor/mic_monitor`, `mic_level`, `mic_active`.
- `grab_mic_to_pad(int pad0_7, double seconds, std::string& error)`: read the last `seconds` frames from `mic_history_`, downmix to mono, peak-normalize to 0.9 (skip if peak < 0.001), build a `WavData` at the engine sample rate, load via new `DrumKit::load_override_data` (B4). Returns false + reason when history is short or the pad is invalid.
- `test_feed_mic(l, r, n)` test hook mirroring `test_feed_note`.

### B4. DrumKit
- `drum_kit.hpp/.cpp`: `bool load_override_data(int midi_note, WavData const&, std::string&)` — mirror of `load_override` (`drum_kit.cpp:662`) calling the active pack sampler's `add_zone_data`. The documented zone-swap race (`drum_kit.hpp:23-32`) is unchanged (rare, user-initiated).

### B5. MicPanel (new `src/ui/mic_panel.{hpp,cpp}`)
Compact channel-strip panel, placed in the **mid row** (`main_window.cpp:532-541`) as a fixed-width column between the arrangement canvas and the right edge — the canvas gives up width gracefully, and the bottom row stays uncluttered:
- status dot (green = frames flowing, dim = no capture device/failed),
- MONITOR toggle chip, GAIN lane (0..200%) with value readout, live level bar,
- GRAB row: duration chips (1s/2s/4s), 8 target-pad chips (bank A, note 36..i), big grab button; result/error line ("→ pad 3 · 2.0 s" or reason), pulses the pad in the mirror on success.
- MainWindow: construct + `mic_panel_->refresh()` in `tick()`.

---

## Tests

- **`tests/test_capture.cpp` (new, registered in `tests/CMakeLists.txt` foreach)**: CaptureRing push/pop ordering, drop-on-full accounting, underflow→0; HistoryRing overwrite-oldest + `read_last` compaction; a two-thread SPSC stress (TSan-covered like the queue tests). PlayEngine end-to-end without hardware: `test_feed_mic` → `render_into` (monitor mix audible in output, level atomic set) → `grab_mic_to_pad` → `drums_.sampler().has_zone(36+pad)` → pad hit renders nonzero.
- **`tests/test_sampler.cpp`**: DrumKit::load_override_data round-trip (build WavData → override → has_zone → render).
- **`tests/test_mapping.cpp`**: `knob_action` table coverage incl. new KIT bindings (audit table stays green).

## Docs

`CHANGELOG.md` (v0.1.5 entry), `README.md` feature bullets, `docs/MANUAL.md` — new "Microphone" section (monitoring, feedback warning, grab-to-pad workflow, JACK port names, ALSA device fallback) + updated control reference; `ROADMAP.md` tick.

## Verification

1. `cmake --build build` clean, then `ctest --test-dir build` — all suites pass (incl. new `capture`).
2. Run the app (ALSA fallback or PipeWire) and confirm in the log: backend capture lines (`mic: ...`), no xrun storm.
3. UI sanity via the existing `A94_SHOT`/`A94_SHOT_PLAY` screenshot hooks (`app_gui.cpp:67-82`): drag a mirror knob (value + engine response), toggle an FX chip dot, mute a strip, mic panel renders.
4. Mic path without a real mic: `test_feed_mic` exercises the exact RT/grab code; on the desktop, monitoring with any PipeWire source (even a tone) audibly mixes.

Implementation order: mapping/actions → PlayEngine (mixer/mute/arm/KIT knobs/velocity) → capture rings → backends → PlayEngine mic + grab → DrumKit → UI (mirror, strips, fx, health, transport, MicPanel) → tests → docs.