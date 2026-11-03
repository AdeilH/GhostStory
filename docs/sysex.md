# MPK Mini SysEx — findings & methodology

Status: **living document** — updated whenever `addu94 probe --capture`
produces new evidence. Facts must be backed by captures under
`~/.local/share/addu94/devices/captures/` (kept per-model); no folklore.

## What we can confirm today (2026-11-03, no hardware attached in dev env)

- The MPK Mini series is USB class-compliant; on Linux it appears as an ALSA
  sequencer client (e.g. client name "MPK mini 3") with a duplex port.
- Units respond to the standard **MIDI Identity Request**
  `F0 7E 7F 06 01 F7` on mk2/mk3 (mk1 historically does not); the reply
  `F0 7E <dev> 06 02 <mfg> <family:2> <model:2> <version:4> F7` gives
  family/model/version. Our probe records the full reply in the manifest's
  `firmware` field when observed. If a unit does not answer, the manifest
  marks `identity_request_supported=false` and the model guess comes from
  the ALSA client name alone.
- Community RE resources (documented for the M4 preset work, to be verified
  per-unit with capture): mk2/mk3 preset dumps use an Akai SysEx header of
  the form `F0 47 <device id> 7A <length> <payload> ... F7` (manufacturer
  0x47 = "inMusic/Akai"). The exact per-preset byte layout varies by model
  and firmware — **do not hardcode until a capture proves it.**

## Capture methodology

1. `addu94 probe --capture` runs a 60 s capture of every message the unit
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
