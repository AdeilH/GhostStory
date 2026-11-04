# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

## [Unreleased]

### Added

- **Four more pad packs** (Boom Bap, Jungle, Trap, House — 8 built-ins
  total): same 8 DSP recipes, new parameter tables; a new test asserts
  every pack pair renders measurably distinct kicks, so a future pack can
  never ship as a renamed copy.

### Fixed — user reports: "knobs do nothing", "keys stick on one output",
"can't work out how to use different packs with the pads"

- **Knobs**: the mapping engine now recognizes ALL three stock MPK knob CC
  layouts by position — CC 1–8, CC 21–28 and CC 70–77 (it only knew 1–8).
  `knobs.N.cc` profile entries still override. Unmapped CCs log one line
  per CC number per process pointing at `addu94 calibrate` /
  `profile.toml`.
- **Knobs do things now**: PlayEngine implements every knob action —
  TrackVolume/TrackPan as per-track render gain (equal-power pan),
  FilterCutoff/FilterReso/EnvAttack/EnvRelease/InstrumentMacro1 pushed to
  the selected track's synth through an RT-safe parameter queue,
  MasterVolume (0..1.5), ClickLevel (metronome gain), QuantizeGridSet
  (off / 1/8 / 1/16 / 1/32 zones — Grid gained ThirtySecond). Honest
  limitations, documented: TrackSend is stored for the FX-send milestone
  (fx.hpp has no send bus yet), ScrubCoarse/Fine are stored until
  Transport gains a position-set control.
- **Profile loading**: `profile.toml` (from `addu94 calibrate`) is now
  actually loaded — project dir first, then `$XDG_CONFIG_HOME/addu94/`,
  then `~/.config/addu94/`; the resolved knob CC map is logged at startup.
- **Keyboard routing**: 4 tracks, each its own sound — Keys A (default
  patch), Keys B (square lead), Drums (keyboard notes map to pads mod-8,
  any octave), Bass (−12, mono-ish sub). The keys always play the SELECTED
  track (arp + chord slots follow). New API: `track_count()`,
  `track_name()`, `track_role()`, `selected_track()`, `select_track()`,
  `track_volume()`, `track_pan()`, `track_send()`, `TrackRole`.
- **Pad packs**: DrumKit now ships 4 built-in packs — "MPC Classic"
  (previous recipes, bit-identical), "808", "Lo-Fi", "Techno" — the same
  DSP with different parameter tables, all synthesized at init. Pack
  switching is a single atomic index (race-free against the audio
  callback; zone-table mutation while rendering was not). New API:
  `pack_count()`, `pack_name()`, `load_pack()`, `current_pack()`,
  `PlayEngine::pad_packs()/set_pad_pack()/cycle_pad_pack()`.
- **Synth**: the per-voice extension state moved from a process global
  into the Synth instance — running 3 synths concurrently (the new track
  model) would otherwise clobber unison/filter state across tracks.
- **VoiceCmd queue** is now MPMC: live keys (MIDI thread) and arp/chord
  strikes (app thread) are genuinely two producers — the old SPSC could
  corrupt under that overlap.
- New tests: knob alias sets / override / unmapped-CC (test_mapping),
  pack names / switching / per-pack audibility / determinism
  (test_sampler). 20/20 suites green.

## [0.1.3] — 2026-11-04

### Fixed — pad semantics (user report: "pad 1 and pad 2 have same response")
- Root cause: A1+A2 was the Undo chord — drumming the two pads in sequence
  fired Undo (erasing takes!), and the ChordSlot/ClipLaunch actions pads
  were mapped to were never implemented, so pads had no distinct response.
- New PLAY-mode pad semantics, all immediate and distinct:
  - **A1–A8 (notes 36–43): drum pads** — 8 distinct kit sounds, instant.
  - **B1/B2 (44/45): undo / redo** — instant, dedicated pads.
  - **B3–B8 (46–51): chord memory slots** — tap plays the stored chord
    (note-ons on press, note-offs on release); hold ≥400 ms while playing
    keys captures the current keys into the slot.
  - EDIT mode: A1–A8 selects tracks (press, was release).
- No two-pad chords anywhere: drumming can never erase a take.
- Mirror legend line ("A drums · B1/B2 undo/redo · B3-B8 chords
  (hold+keys=capture)") so the functions are discoverable on-screen; each
  pad pulses its own action in the mirror.
- Coverage audit updated (all controls still bound — zero dead controls).
- New test suite test_mapping.cpp (7 tests) incl. the exact regression:
  drumming A1→A2 produces two distinct DrumPad hits and never Undo.
- 20 test suites green; make ci ALL GATES GREEN.

## [0.1.2] — 2026-11-04

### Added — UI modernization ("one screen = the instrument")
- Single-screen layout replacing the tabbed UI: transport bar (drawn
  play/stop/rec/metronome icons, shortcut hints), left track rail with
  inline mute/arm/volume/pan per track, central arrangement canvas
  (section bands, per-track-hue pattern blocks, drag-to-move with bar
  snap, double-click to extend, glowing playhead + trail, loop shading),
  restyled controller mirror with segmented mode control, health strip
  with device LED, live xrun/callback readouts and an honest master
  level meter (RT-safe peak tap in PlayEngine).
- Musician interactions: Space = play/stop, R = record, M = metronome;
  drag BPM/loop values; tooltips everywhere.
- Permanent UI test aid: A94_SHOT=<path> grabs an offscreen screenshot
  (A94_SHOT_PLAY=1 starts transport for live-state captures).

### Fixed
- **Mix bug** (found via the UI meter debug): Sampler::render overwrote
  buffers, silently wiping the synth when drums played through PlayEngine
  — sampler now accumulates into caller-zeroed buffers (engine contract);
  verified synth+drums mix (kick+snare RMS over sustained synth).

### Verified
- 30-minute soak (spec §11, full duration): PASS — 0 xruns, 35,609 note
  events journaled and recovered, fuzz survived.
- UI verified through 6 offscreen screenshot iterations; 19 test suites;
  make ci ALL GATES GREEN.

## [0.1.0] — 2026-11-04

Milestones M0–M8 complete. Highlights across the release:

- **Instruments**: subtractive synth (2 osc + unison, ADSR, SVF, LFO),
  WAV sampler (zones, per-pad tuning/decay, choke groups), drum kit with
  synthesized CC0 fallbacks — playable with zero external files.
- **Workflow**: loop recording with quantize, take overdub, per-take undo;
  scale-lock; software arp (6 modes, octave stack); pattern clips +
  arrangement blocks/sections; crash-safe project persistence (WAL
  journal, per-append fsync, atomic saves, quarantine).
- **Native Qt6 UI** (dark, hardware-first): controller mirror, transport
  bar with live health readouts, Arrangement/Pattern/Mixer/Device/Health
  views; runs offscreen for CI.
- **Ops**: watchdog with structured incidents, crash handler + safe mode,
  local-only opt-out telemetry, doctor/repair/bench/probe/export CLI,
  MIDI fuzz harness (spec §7.5) and kill -9 recovery as CI gates.
- **Gates (all green at tag)**: 18 test suites; CI = build → ctest →
  ASan/UBSan → TSan → kill -9 recovery → MIDI fuzz → clang-tidy; live
  bench: p99 callback 6 µs (0.1 % of budget), 0 xruns / 5 min.
- **Live-verified on MPK mini 3**: probe/identity (mfg 0x47, fw 1.1.8),
  pads on ch 9, keys→synth→speakers, export SMF+WAV.

Deferred (tracked in ROADMAP.md): JACK/ALSA backend implementations
(interface shipped, PipeWire is the production path), FLAC export (WAV
shipped), hardware ARP/preset SysEx editing, on-hardware acceptance
checklist (§12 items 1–6 need the owner's hands on the device).
