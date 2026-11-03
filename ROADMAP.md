# ROADMAP

Milestones per BUILD PROMPT §10, in order, with acceptance gates.
Status: ☐ todo · ◐ in progress · ☑ done

- ☑ **M0 — Scaffolding & ADRs** — repo, CMake+Ninja, CI gate, ADRs, logging.
  *Gate: `make ci` green.* **DONE 2026-11-03.**
- ☑ **M1 — MIDI engine + probe** — ALSA seq client, timestamps, device
  detection, manifest, SysEx capture.
  *Gate: unit + virtual-port tests; probe writes valid manifest.* **DONE
  2026-11-03** (first live detection: MPK Mini MK3, client 20; identity
  request answered=no — verify on-device during M4 work).
- ☐ **M2 — Audio engine** — PipeWire out (JACK/ALSA fallback), metronome,
  sample-accurate transport. *Gate: p99 callback < 30% buffer, 0 xruns/5 min.*
- ☐ **M3 — First playable** — keys→synth, sustain, octave, velocity curves,
  mixer, transport, loop record. *Gate: record/overdub/undo round-trip test.*
- ☐ **M4 — Full control system** — modes, knobs, pads, joystick, ARP (hw+sw),
  note repeat, controller mirror UI. *Gate: control-coverage audit, 0 dead controls.*
- ☐ **M5 — Song builder core** — tracks, patterns, arrangement, quantize,
  scale-lock, WAL + crash recovery. *Gate: kill -9 recovery; SMF export valid.*
- ☐ **M6 — Sound completeness** — sampler (SFZ/WAV), drum kit, FX, WAV/FLAC
  export. *Gate: master = sum of stems (tolerance).*
- ☐ **M7 — Ops layer** — watchdog, doctor, repair, safe mode, crash handler,
  telemetry, bench, fuzz. *Gate: fuzz green; forced faults recover.*
- ☐ **M8 — Polish & packaging** — docs, man page, .desktop, packaging,
  onboarding, v0.1.0. *Gate: fresh-machine checklist.*
