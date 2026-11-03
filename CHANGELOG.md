# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

## [0.1.0-dev] — 2026-11-03 (M2 + M3)

### Added
- **Audio engine**: PipeWire-native playback backend (pw_stream, RT-safe
  callback, measured 5-min bench: 28125 callbacks, 0 xruns, p99 = 6 µs =
  0.1% of buffer budget — gate PASS); backend factory ready for JACK/ALSA
  fallbacks.
- **Transport**: sample-accurate, integer position, buffer-boundary tempo
  changes, exact loop wrap, control-plane state requests (play/stop/record
  via atomics, never cross-thread plain writes); metronome with
  bit-deterministic click synthesis; 9 property tests on clock math.
- **Subtractive synth**: 2 osc + unison(3) w/ constant-power pan, ADSR,
  Chamberlin SVF (LP12/LP24/HP12/BP12) w/ keytrack, LFO→filter/pitch/amp,
  voice stealing, click-free release, panic; agent-built + selftested
  (extreme-params stable), 5 in-tree tests.
- **Mapping layer**: modes (PLAY/EDIT/STEP), knob CC bindings from
  profile.toml (default profile embedded TOML parsed by our own parser),
  pad actions fire on release, two-pad chords (undo/redo, mode switches),
  controller-mirror state; minimal TOML parser (fuzz-lite tested).
- **First playable** (`addu94 play`): MPK keys → velocity-curved synth →
  speakers; pads → chord slots; knobs → mix/synth; sustain pedal CC64;
  loop recorder with quantize-on-record, take-based overdub + per-take
  undo (M3 gate: record/overdub/undo round-trip tests green).
- Music tools: velocity curves (log default — mini keys), quantizer
  (1/4..1/16T, strength), scale-lock tables (constexpr), arp
  (up/down/up-down/random/order/chord, octave stack) + note repeat.
- Project layer: document JSON serialization, WAL journal (per-append
  fsync, corrupt-tail tolerance, atomic save w/ rotate), 16 tests incl.
  crash simulations.

### Fixed
- TSan: probe identity flag raced across threads (now atomic) — caught by
  gate on first run with the new probe flow.
- Test framework: CHECK_EQ bound references to temporaries' elements
  (dangling) — now copies; a latent garbage-read class of test bugs gone.

## [0.1.0-dev] — 2026-11-03 (M1)

### Added
- **MIDI engine** (`src/midi/`): ALSA sequencer client with dedicated input
  thread (poll-based, non-blocking drain), heartbeat for the future
  watchdog, event/hotplug decode to fixed-size RT events, ALSA↔14-bit pitch
  bend conversion, SysEx passthrough (raw sink + inline truncation for RT).
- **Device layer** (`src/device/`): manifest model (checksummed SHA-256,
  schema-versioned JSON via vendored nlohmann), default control inventory
  (55 controls), atomic save (temp+fsync+rename), corrupt-file quarantine,
  runtime probe with MIDI Identity Request and raw capture
  (`addu94 probe [--capture]`).
- First live-device detection: MPK Mini MK3 discovered on ALSA client 20;
  manifest written to `~/.local/share/addu94/devices/`.
- `docs/sysex.md` — capture-first methodology; no hardcoded model bytes.
- Tests: virtual-port round trip, monotonic timestamps, model matching,
  manifest round-trip/checksum/garbage, probe-without-hardware (7 tests,
  zero physical hardware required).

### Fixed
- clang-tidy gate: Debian ships with no default checks; explicit curated
  set; GCC module-scanner flags in the compile db documented as noise.
- CI ASan/UBSan link: main binary links sanitizer runtime (instrumented
  core lib requires it).

## [0.1.0-dev] — 2026-11-03 (M0)

### Added
- Repository scaffold: CMake ≥3.28 + Ninja build, warnings-as-errors,
  two-compiler matrix (GCC 16 / Clang 24, C++26 — ADR 001).
- ADRs 001 (C++26), 002 (ALSA seq + PipeWire native stack), 003 (Qt6 UI).
- `src/core/spsc_queue.hpp` — wait-free lock-free SPSC ring buffer,
  cache-line isolated, TSan-tested in CI.
- `src/log/` — async structured logger: allocation-free RT producer, JSONL
  + human sinks, bounded entries, drop-counting under pressure.
- Dependency-free test framework + first tests (SPSC correctness/concurrency,
  logger JSONL/escaping/bounds/concurrency).
- `addu94` CLI skeleton: `version`, `doctor` (honest M7 stub), usage.
- CI gate `make ci` (scripts/ci.sh): clean build → ctest → ASan/UBSan →
  TSan → clang-tidy (best-effort).
- GARDENING.md (binding self-improvement loop), ROADMAP.md, INCIDENTS.md.
