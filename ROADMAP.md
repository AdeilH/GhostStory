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
- ☑ **M2 — Audio engine** — PipeWire out (JACK/ALSA fallback), metronome,
  sample-accurate transport. *Gate: p99 callback < 30% buffer, 0 xruns/5
  min.* **DONE 2026-11-03** (measured: p99=6 µs = 0.1% of budget, 0 xruns;
  JACK/ALSA backends deferred as drop-in interface impls).
- ☑ **M3 — First playable** — keys→synth, sustain, octave, velocity curves,
  mixer, transport, loop record. *Gate: record/overdub/undo round-trip
  test.* **DONE 2026-11-03** (live-verified on MPK mini 3).
- ☑ **M4 — Full control system** — modes, knobs, pads, joystick, ARP
  (sw; hw-arp reachable via SysEx presets in M6+), note repeat (sw),
  controller mirror UI (native Qt6), coverage audit. *Gate: audit passes —
  zero dead controls.* **DONE 2026-11-04.**
- ☑ **M5 — Song builder core** — tracks, patterns, arrangement, quantize,
  scale-lock, WAL + crash recovery. *Gate: kill -9 recovery (31 events
  recovered after SIGKILL); SMF export valid.* **DONE 2026-11-04.**
- ☑ **M6 — Sound completeness** — sampler (WAV zones; SFZ parser deferred),
  drum kit w/ synthesized fallbacks, FX chain (EQ3/delay/chorus/clip),
  WAV stems+master export. *Gate: master = sum of stems (1e-6).* **DONE
  2026-11-04.** FLAC export deferred (no libFLAC dependency accepted for
  offline-first build; WAV is the shipped format).
- ☑ **M7 — Ops layer** — watchdog, doctor, repair, safe mode, crash
  handler, telemetry, bench, fuzz. *Gate: fuzz green (1119 blobs, no
  wedge); kill -9 recovery green.* **DONE 2026-11-04.**
- ☑ **M8 — Polish & packaging** — docs (MANUAL.md), man page, .desktop,
  PKGBUILD, install rules, v0.1.0 tag. *Gate: full CI green at tag.*

## Post-v0.1.0 backlog (honest deferrals)

- ☑ SFZ parser (practical v1 opcode subset) — DONE post-tag: sfz.hpp/.cpp,
  13 tests, feeds Sampler directly.
- ☑ UI modernization — DONE post-tag: single-screen instrument layout,
  drag/hover/keyboard interactions, master meter; screenshot-driven
  development (6 iterations); fixed the drums-vs-synth mix bug found by
  the meter debug.
- ☑ 30-min soak at full spec duration — PASS (0 xruns, 35609 events).
- ☑ Knob calibration `ghost_story calibrate` — DONE post-tag: interactive
  CC mapping -> profile.toml (makes all 8 knobs work on stock presets).
- ☑ Trace mode `ghost_story bench --trace` — DONE post-tag: Chrome-trace JSON
  of callback timings (Perfetto/chrome://tracing loadable).
- ☑ Soak gate scripts/test_soak.sh — DONE post-tag: concurrent record +
  fuzz + bench; 2-min validation PASS (0 xruns, 2357 events recovered);
  `test_soak.sh 30` runs the full spec §11 duration.
- ☑ Live per-track FX + FX-send — DONE post-tag (was the "FX-send
  milestone" honest limitation): per-track insert chains run in the audio
  callback, send knob = reverb amount; 5 new blocks (comp, crush,
  flanger, tremolo, reverb) + hard/fold drive shapes; EDIT knobs became
  the FX page; FxPanel UI + 10 curated presets.
- ☑ 16 pad packs (Acoustic, Linn 80s, DnB, Dub, Ambient, Electro,
  Garage, Breaks added) + synth mono-legato/glide, velocity->filter,
  second LFO.
- ☑ Live mic input (monitor + grab-to-pad) — DONE post-tag: capture paths
  in all three backends (PipeWire capture stream / JACK in_L,in_R / ALSA
  capture thread), lock-free CaptureRing + ~11 s HistoryRing, pre-fader
  monitor mix, MicPanel with 1/2/4 s peak-normalized grabs into bank-A
  pads (`DrumKit::load_override_data`).
- ☑ Every on-screen control wired — DONE post-tag: interactive mirror
  knobs/pads (same command path as hardware via the hoisted
  `knob_action()` table), real TrackStrips mixer (volume/pan/mute/arm),
  FX chip toggle, velocity-curve combo, master fader, command-route
  transport buttons, KIT sound-design knob page (curve/gain/tune/decay).
- ☐ JACK / ALSA audio backends — IN PROGRESS (live-tested against
  PipeWire's JACK emulation on this machine; capture sides now exist on
  all three — playback fallbacks await multi-machine testing).
- ☐ FLAC export — BLOCKED (no sudo on dev machine to install libflac-dev;
  WAV is the shipped format).
- ☐ Hardware ARP + preset SysEx editing (capture methodology in
  docs/sysex.md; needs on-device sessions).
- ☐ On-hardware acceptance checklist (spec §12 items 1-6) — requires the
  owner playing the device; automated gates cover everything scriptable.
- ☐ Transport position-set control (the scrub knobs were traded for the
  FX page; the stored scrub position becomes live the moment this lands).
- ☐ Shared FX send bus (Reverb already exists standalone in fx.hpp for
  exactly this; the insert chains are the current, shipped shape).
- ☐ Song timeline — MULTIPLE clips per track chained end-to-end (add /
  clone / move / delete, transport plays the chain). Shipped meanwhile:
  the playback engine, dynamic tracks, one PLACEABLE clip per track
  (drag/resize/clear, `ClipMove` journal op), playhead seek. The
  document format stores per-track clip vectors, so persistence needs no
  schema change.
