# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

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
