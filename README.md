# Ghost Story

A self-healing, self-improving **song builder** for Linux, purpose-built for
the **Akai MPK Mini** (mk1 / mk2 / mk3 / MPK Mini Play — detected at runtime).

No AI at runtime. No cloud. No accounts. Deterministic algorithms, honest
latency budgets, and every physical control on the device bound and remappable.

- **Status:** v0.2.0-dev — see [ROADMAP.md](ROADMAP.md)
- **Contract for development sessions:** [GARDENING.md](GARDENING.md)
- **Architecture decisions:** [docs/adr/](docs/adr/)

## Highlights

- Four-track groovebox (Keys A / Keys B / Drums / Bass) with per-track
  insert FX chains and ten curated presets.
- **Live microphone input** — monitor your voice over the beat while it
  runs, and **grab the last 1/2/4 s straight into a drum pad** as a new
  sample (PipeWire capture stream / JACK input ports / ALSA capture).
- The on-screen controller mirror is **playable itself**: drag its knobs,
  click its pads, mute/arm/volume/pan the tracks, toggle FX chips — every
  control drives the engine through the same command path as the hardware.
- Full loop recorder (quantized takes, overdub, undo/redo), 16 built-in
  drum-kit packs, sample-accurate transport, WAL project persistence.
- Three audio backends (PipeWire → JACK → ALSA) with lock-free RT mixing.
- **Four themes** — Graphite (default), Midnight, Slate, Paper — switchable
  via the labeled THEME dropdown in the health strip, remembered across
  runs. Window resizes freely (960×600 floor) and F11 goes fullscreen.

## Quick start (developers)

```sh
make ci        # full gate: build + tests + ASan/UBSan + TSan
make build     # fast local build -> build/main/ghost_story
./build/main/ghost_story --help
```

## User quick start (when installed)

```sh
Ghost Story doctor   # one-command health check
Ghost Story probe    # detect your MPK Mini, write the device manifest
Ghost Story play     # make music
```

## Layout

| Path | Layer |
|---|---|
| `src/core/` | RT primitives (lock-free SPSC queue) |
| `src/log/` | async structured logger (RT-safe) |
| `src/midi/` | ALSA sequencer engine (M1) |
| `src/device/` | manifests, probe, SysEx (M1) |
| `src/mapping/` | CC/note/pad bindings, modes (M4) |
| `src/music/` | transport, quantizer, arp (M2–M5) |
| `src/audio/` | PipeWire/JACK/ALSA backends, DSP (M2–M6) |
| `src/project/` | document model, undo, WAL (M5) |
| `src/ui/` | Qt6 app (M4+) |
| `src/ops/` | watchdog, doctor, telemetry (M7) |
| `tests/` | dependency-free tests, virtual-MIDI only |
| `tools/` | dev-time tooling (fuzzers, report analysis) |
| `deploy/elk/` | dev observability stack (ES + Kibana + Filebeat; `make elk-up`) |
