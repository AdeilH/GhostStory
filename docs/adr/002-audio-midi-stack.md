# ADR 002 — Audio & MIDI Stack: ALSA seq + PipeWire native, hand-rolled

- **Status:** Accepted
- **Date:** 2026-11-03

## Context

Ghost Story needs: (a) raw MIDI from the MPK Mini with low, measurable latency;
(b) audio output via PipeWire with JACK/ALSA fallbacks; (c) strict realtime
safety in the audio callback; (d) virtual MIDI ports for hardware-free tests.

## Decision

1. **MIDI:** talk to ALSA **libasound sequencer** directly (`snd_seq_*`),
   plus `snd_rawmidi` where the probe/SysEx path needs byte-exact I/O.
   Timestamps come from the ALSA event timestamps converted to our transport
   clock. Virtual ports (`snd_seq_create_simple_port` with
   `SND_SEQ_PORT_CAP_DUPLEX` on a userspace client) are the test rig — every
   automated test runs against them; no physical MPK required.
2. **Audio:** **PipeWire native API** (`libpipewire-0.3`, have 1.6.9) as the
   default graph; **JACK API** fallback (libjack present); **ALSA PCM**
   last resort. Backend selected at runtime by probing availability, overridable
   via `~/.config/ghost_story/config.toml`.
3. **DSP & synth:** hand-rolled, self-contained (spec §4). No external synth
   hosting. SFZ parsing and sample loading are built in-house over M6
   (`sfizz` as a library may be adopted only if a vendored, offline build is
   practical — decision deferred to M6 ADR).
4. **No JUCE.** JUCE would bundle its own audio device layer, fighting our
   mandated layering (midi-engine → device → mapping → music → audio) and its
   GPL/commercial licensing complicates packaging. Hand-rolled core + Qt6
   widgets for UI (see ADR 003) keeps every layer replaceable and testable.

## Consequences

- More code owned by us (transport, backends) — mitigated by golden-file and
  virtual-port tests, and by strict one-layer-per-directory separation.
- PipeWire native gives us graph introspection (node names, xrun counts)
  that JACK emulation would hide, which the ops layer (doctor/bench) needs.
