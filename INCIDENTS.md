# INCIDENTS

Open, severity-ordered incidents. A "gardener" session fixes the top item,
adds a regression test, then moves it to Resolved. (Format per GARDENING.md.)

## Open

_(none yet — record format: `## INC-<n> <title> (severity S0–S3)` with
symptoms, evidence path, suspected layer, owner session)_

## Resolved

### INC-1 Phantom TSan races from uninstrumented core lib (S2) — RESOLVED 2026-11-03
- **Symptom:** `test_logger` reported consistent data races inside
  `MpmcQueue::pop` under TSan; standalone repros of the same code were clean;
  Clang TSan unavailable (runtime not installed).
- **Root cause:** `addu94_core` static library was compiled without
  `-fsanitize=thread` while test TUs were instrumented. Uninstrumented
  atomics in `logger.cpp` broke TSan happens-before tracking, producing
  phantom races (and could equally hide real ones).
- **Fix:** apply `addu94_apply_sanitizers` to `addu94_core` whenever
  `ADDU94_SANITIZE` is on (CMakeLists.txt).
- **Regression test:** any sanitizer test build; the build config itself is
  the fix (verified: test_logger/test_spsc/test_mpmc all exit 0 under TSan).
