# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

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
