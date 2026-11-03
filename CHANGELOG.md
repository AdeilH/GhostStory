# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

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
