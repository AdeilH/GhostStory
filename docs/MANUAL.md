# addu94 — User Manual (v0.1.0)

## First run

1. Plug in your MPK Mini (any mk1 / mk2 / mk3 / MPK Mini Play).
2. Run `addu94 probe` — writes your device manifest (model, ports,
   55-control inventory, checksummed).
3. Run `addu94 doctor` — all checks should read `[ OK ]` (warnings about
   realtime limits are informational).
4. Run `addu94 play` — the native window opens. **Play the keys.**

## The window

- **Transport bar**: play/stop/record, BPM, loop bars (0 = off), click
  level, live position (bar.beat.tick), xrun + callback-time readouts,
  device LED.
- **Controller mirror** (bottom, always visible): the 8 knobs with live
  values, 16 pads (banks A/B, hits glow), mode strip. Click PLAY/EDIT/STEP
  to switch modes. Everything the hardware does is mirrored here.
- **Views**: Arrangement (timeline with playhead), Pattern, Mixer, Device
  (manifest + knob table + velocity curve), Health (stats + in-app bench).

## Making a loop

1. Press **Record** (transport bar or the hardware path: Mode EDIT → pads).
2. Hit pads (channel 10 — the MPC convention) — hits are quantized to the
   1/16 grid, per-take overdub, two-pad chord A1+A2 = undo last take.
3. Keys play the subtractive synth (log velocity curve by default — mini
   keys need it). Sustain pedal works. Two-pad chords: A1+A8 → EDIT mode.
4. Stop saves the project (WAL journal — kill -9 loses at most ~1 s).

## Sound

- Track 1: subtractive synth (2 osc + unison, ADSR, SVF filter, LFO).
- Pads: drum kit — 8 synthesized CC0 fallback sounds out of the box;
  override per pad with your own WAVs later (`docs/`).
- Knobs (PLAY mode): 1 volume · 2 pan · 3 send · 4 macro · 5 filter
  cutoff · 6 resonance · 7 attack · 8 release.

## Export

```sh
addu94 export --project-dir ~/.local/share/addu94/projects/default \
    --smf song.mid          # Standard MIDI file (type 1)
addu94 export --project-dir ... --wav-dir stems/   # stems + master WAV
```

The master mix equals the sum of stems (tested to 1e-6).

## Troubleshooting

- **No sound**: `addu94 doctor` → audio backend line. PipeWire required
  (JACK/ALSA fallbacks planned; see ROADMAP).
- **After a crash**: reports are in `~/.local/share/addu94/crashes/`; the
  next launch tells you. Run `addu94 repair`, then `addu94 doctor`.
- **Knobs do nothing**: run `addu94 probe` — the stock MPK programs map
  knobs to CCs that differ per preset; the calibration flow lands in the
  Device view (see ROADMAP M4+ notes).
- **Privacy**: telemetry is local-only; `ADDU94_TELEMETRY=0` disables it
  entirely.
