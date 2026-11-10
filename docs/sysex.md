# MPK Mini SysEx — findings & methodology

Status: **living document** — updated whenever `ghost_story probe --capture`
produces new evidence. Facts must be backed by captures under
`~/.local/share/ghost_story/devices/captures/` (kept per-model); no folklore.

## What we can confirm today (2026-11-03 — live MPK mini 3, USB full speed)

- **Identity Reply confirmed** (probe + capture):
  `F0 7E 7F 06 02 47 49 00 19 00 01 01 08 00*5 41 31 32 31 30 32 32 34 38 37 32 34 35 31 39 00 F7`
  → manufacturer `0x47` (inMusic/Akai), family `0x4900`, model `0x1900`,
  version `1.1.8`, ASCII payload = device serial (`A12102248724519`).
  Note: the reply only arrives when the device's output port is subscribed
  to our input — `ghost_story probe` now does both directions.
- **Pads confirmed**: MK3 pads send Note On/Off on **channel 9** (MIDI ch 10)
  — bank A pad 3 = note 38, full-strike velocity 127. Inventory updated.
- **Knobs observed**: CC#2 stream while untouched (repeating values ~16-34,
  ~every 5 s) — knob 3 pot appears noisy/uncentered or auto-reporting.
  CC#5, CC#6 seen under user turn. Default-program knob CC map is NOT
  70-77 as assumed from other MPK models — full per-knob mapping deferred
  to the M4 onboarding calibration (turn each knob, record the CC).
- Announce-port control events (client/port start-change) arrive as
  SND_SEQ_EVENT types outside the note/CC space — decoded to `Other`
  intentionally (they are system events, not device data).

## Community RE resources (to verify with capture, not yet confirmed)

## Capture methodology

1. `ghost_story probe --capture` runs a 60 s capture of every message the unit
   emits (see `CaptureRecord`), including unrecognized ones.
2. For preset dumps: press each hardware preset button in turn during a
   capture; diff captures to isolate the preset SysEx block.
3. For knob/pad mappings: turn one knob / hit one pad per capture window;
   diff to derive the CC/note address space empirically.
4. Store the anonymized capture (strip nothing — it's local-only) with the
   manifest; when a format is decoded, encode the knowledge as a
   `device_manifest.json` capability + a section here, never as magic bytes
   in engine code.

## Open questions (verify per connected unit)

- [ ] Does the connected unit expose aftertouch/channel pressure on pads?
- [ ] Are knobs absolute pots (mk1/mk2) or relative encoders (mk3)?
- [ ] LED feedback protocol for pads on the connected model?
- [ ] Preset dump/restore SysEx layout for the connected model?
