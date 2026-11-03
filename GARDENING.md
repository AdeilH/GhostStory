# GARDENING.md — the addu94 self-improvement loop

This file is **binding** on every development session (human or agent). Each
session is one gardening iteration. Do not change user-visible behavior
without a regression test. Commit fixes with a `gardener:` prefix.

## The loop (run in order, do not skip)

1. **Diagnose:**
   ```sh
   make ci            # build + tests + ASan/UBSan + TSan gates (must be green before you start)
   ./build/main/addu94 doctor     # when M7 lands; until then, run make ci
   ./build/main/addu94 bench      # when M2/M7 lands
   ```
2. **Read the evidence:**
   - `~/.local/share/addu94/telemetry/` (JSONL sessions; when M7 lands)
   - `~/.local/share/addu94/crashes/` (minidumps + logs)
   - `INCIDENTS.md` — open incidents, severity-ordered
   - `ROADMAP.md` — milestone status and next work
   - `bench-report.json` regressions > 10% = a bug, not a footnote
3. **Pick ONE item:** the highest-severity open incident, else the highest
   value open roadmap item. One item per iteration.
4. **Fix it.** Add or extend a regression test that fails before the fix and
   passes after. Tests run on virtual MIDI ports / in-process — no hardware
   in the loop, ever.
5. **Verify:** `make ci` green. If bench regressed >10%, the fix is not done.
6. **Record:**
   - `CHANGELOG.md` — what changed
   - `INCIDENTS.md` — move resolved items to "Resolved", with evidence
   - `ROADMAP.md` — update milestone status
   - Commit: `gardener: <one-line summary>` referencing the test name.
7. **Hand off:** ensure GARDENING.md is still accurate for the next session.
   If the loop changed, update this file in the same commit.

## Hard rules

- **No AI at runtime.** Runtime intelligence = deterministic rules in
  `rules.toml` + statistics. You (the agent) are dev-time only.
- **RT path stays boring:** no allocation, locks, syscalls, or exceptions in
  the audio callback; all RT cross-thread traffic via `src/core/spsc_queue.hpp`
  (TSan-covered).
- **No silent data loss:** journal first (project WAL), then save atomically.
- **No hardcoded MPK model bytes:** everything device-specific goes through
  the manifest layer (`~/.local/share/addu94/devices/device_manifest.json`).
- Tests must pass with **no physical hardware** — ALSA virtual ports only.
- Never leave the tree red; revert rather than commit broken.
