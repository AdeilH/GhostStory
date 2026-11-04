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

- **Four tracks — the keys always play the SELECTED track** (see
  *Keyboard routing* below):
  - **Keys A** — subtractive synth (2 osc + unison, ADSR, SVF filter, LFO),
    the shipped default patch.
  - **Keys B** — a second synth voice (square lead: brighter cutoff,
    snappier envelope).
  - **Drums** — the drum kit; keyboard notes map into the pads, so any
    octave plays drum sounds.
  - **Bass** — a dedicated bass voice (triangle + sine sub, dark filter),
    played an octave down.
- **Pads: drum kit with eight built-in packs** (see *Pad packs* below).
- Knobs (PLAY mode): 1 volume · 2 pan · 3 send · 4 macro · 5 filter
  cutoff · 6 resonance · 7 attack · 8 release — they act on the SELECTED
  track. Knob 3 (send) stores its value for the FX-send milestone; nothing
  is faked in the audio meanwhile.

## Keyboard routing

What the keys play is whatever track is selected:

- Select with the **track strip in the window** (click a track) or in
  **EDIT mode with pads A1–A4** (pad 1 = Keys A … pad 4 = Bass).
- Keys A / Keys B / Bass each have their own synth, so switching tracks
  switches the sound instantly; the Bass track sounds an octave lower than
  what you play.
- With **Drums** selected, the keys become pads: key C2 and every C two
  octaves apart trigger pad 1 (kick), the next seven white/black keys up
  trigger pads 2–8 — the mapping repeats every octave
  (`pad = (note − 36) mod 8`).
- The arpeggiator and chord slots always target the selected track too.

## Pad packs

The pads ship with eight built-in packs — same eight pad slots, different
sounds, zero files needed:

| # | Pack | Character |
|---|------|-----------|
| 0 | MPC Classic | The original synthesized kit (kick sweep 154→44 Hz, tight hats) |
| 1 | 808 | Long sine sub kick, snappy rim, tight short hats, round tom |
| 2 | Lo-Fi | Darker tones, slower decays, softer noise |
| 3 | Techno | Punchy short kick, bright hats, aggressive clap |
| 4 | Boom Bap | Dusty 90s hip-hop: warm mid kick, dry tight snare, dark hats |
| 5 | Jungle | Choppy breakbeat: quick subby kick, bright tight snare, razor hats |
| 6 | Trap | 808 sub with a touch of click, ultra-tight hats, crisp clap |
| 7 | House | Four-on-the-floor: punchy mid kick, disco open hat, wide clap |

Switch packs from the UI (pad-pack control) or programmatically via
`PlayEngine::set_pad_pack()` / `cycle_pad_pack()`; switching is instant and
safe while playing. Your own WAVs can still replace individual pads of the
active pack (`load_override`).

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
- **If the knobs do nothing**: addu94 recognizes all three stock MPK knob
  CC layouts automatically — CC 1–8, CC 21–28 and CC 70–77 map to knobs
  1–8 by position. If your unit was reprogrammed to something else, the
  log prints `unmapped CC <n>=…` once per unknown CC; run
  `addu94 calibrate` (writes `profile.toml` with your real knob CCs) or
  add `knobs.N.cc = <cc>` lines to a `profile.toml` yourself. addu94
  loads it from the project directory first (`<project>/profile.toml`),
  else `$XDG_CONFIG_HOME/addu94/profile.toml`, else
  `~/.config/addu94/profile.toml`; the resolved knob CC map is logged at
  startup either way.
- **Privacy**: telemetry is local-only; `ADDU94_TELEMETRY=0` disables it
  entirely.
