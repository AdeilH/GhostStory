# addu94

A self-healing, self-improving **song builder** for Linux, purpose-built for
the **Akai MPK Mini** (mk1 / mk2 / mk3 / MPK Mini Play — detected at runtime).

No AI at runtime. No cloud. No accounts. Deterministic algorithms, honest
latency budgets, and every physical control on the device bound and remappable.

- **Status:** v0.1.0-dev — see [ROADMAP.md](ROADMAP.md)
- **Contract for development sessions:** [GARDENING.md](GARDENING.md)
- **Architecture decisions:** [docs/adr/](docs/adr/)

## Quick start (developers)

```sh
make ci        # full gate: build + tests + ASan/UBSan + TSan
make build     # fast local build -> build/main/addu94
./build/main/addu94 --help
```

## User quick start (when installed)

```sh
addu94 doctor   # one-command health check
addu94 probe    # detect your MPK Mini, write the device manifest
addu94 play     # make music
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
