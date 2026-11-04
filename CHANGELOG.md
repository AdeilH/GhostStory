# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

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
