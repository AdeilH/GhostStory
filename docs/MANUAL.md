# Ghost Story — User Manual (v0.2)

## First run

1. Plug in your MPK Mini (any mk1 / mk2 / mk3 / MPK Mini Play).
2. Run `ghost_story probe` — writes your device manifest (model, ports,
   55-control inventory, checksummed).
3. Run `ghost_story doctor` — all checks should read `[ OK ]` (warnings about
   realtime limits are informational).
4. Run `ghost_story play` — the native window opens. **Play the keys.**

## The window

- **Transport bar**: play/stop/record (record ARMS — the take starts
  with your first note, from the playhead), BPM, loop bars (∞ = the clip
  keeps looping forever; N = the transport wraps after N bars), click
  level, live position (bar.beat.tick), xrun + callback-time readouts,
  device LED. Stop (button or Space) flushes the recorder and saves the
  project exactly like a hardware stop.
- **Controller mirror** (bottom left, always visible): the 8 knobs with
  live values, 16 pads (banks A/B, hits glow), mode strip, the pad-pack
  dropdown (all 16 packs) and the track chips. Click
  PLAY/EDIT/KIT to switch modes. Everything the hardware does is mirrored
  here — **and it is playable itself**: drag a knob (or wheel-nudge,
  double-click to reset) and it drives the engine through the same
  command path as the hardware; click a pad and it fires per the current
  mode (PLAY drums, EDIT selects/undoes/chords, KIT picks packs).
- **Track strips** (left rail): your tracks (add with **+ add
  instrument**, remove with a right-click) with **real** mixer
  controls — drag the volume lane (0–100%), drag the pan lane, click **M**
  to mute (FX tails keep decaying), click the dot to record-arm that track
  for the loop recorder (nothing armed = everything records, as before).
  Double-click a lane to reset it.
- **FX panel** (bottom center): the selected track's effect chain — 9
  effect chips (the lit ones are on; the filled one is selected), drag
  lanes for the selected effect's two parameters, and the curated preset
  dropdown. **Click a chip's dot corner (or right-click the chip) to toggle
  that effect on/off.** Same state the EDIT-mode FX knobs drive.
- **Mic panel** (right of the arrangement canvas): the live-input channel
  — see *Microphone* below.
- **Health strip** (bottom right): device LED, health readouts, the
  **velocity curve** selector (linear / log / exp / hard-floor — applied
  to every note-on), a **master fader** (drag; 100% = unity), the master
  level meter, and the **theme** picker (Graphite / Midnight / Slate /
  Paper — the whole instrument repaints instantly and the choice is
  remembered across runs). Both selectors carry captions so they are easy
  to spot.
- **Views**: Arrangement (timeline with playhead), Pattern, Mixer, Device
  (manifest + knob table + velocity curve), Health (stats + in-app bench).

## Making music

**Tracks are yours to build.** The app starts with NO tracks: press
**+ add instrument** at the bottom of the track rail and pick from the ten
synth instruments (Default Saw, Acid Bass, Warm Pad, Pluck Keys, Square
Lead, Sub Bass, Dreamy Keys, Stabs, Lo-Fi Keys, Fly Lead — each arrives
with its patch and FX chain preloaded) or the Drum Kit (one shared kit, so
one Drums track). Right-click a track card to remove it. Click a card to
select it — the keys ALWAYS play the selected track.

**Recording starts when YOU play.**

1. Click the arrangement canvas to place the playhead (the dotted line —
   play and record begin there).
2. Press **Record** (transport bar or R) — it arms; nothing rolls yet.
3. Play. The first note starts the transport and the take — everything
   you play lands in the selected track's clip at the sounding pitch, any
   octave, quantized to the 1/16 grid, overdubbed per take (B1/B2 or
   Ctrl+Z / Ctrl+Shift+Z undo/redo). Pads (channel 10) record into the
   Drums track.
4. **Press Space — your take plays back, looping at its length, from its
   spot on the timeline.** Layer more instruments: add a track, select it,
   arm and play.
5. Shape it in the arrangement canvas: **drag a clip** to move it along
   the timeline, **drag its right edge** to resize, **double-click a
   lane** to extend +1 bar, **right-click a clip** to clear its recording.
   Stop saves the project (WAL journal — kill -9 loses at most ~1 s).

The **metronome is opt-in** — M (or the transport button) turns the click
on when you want it; play and record never force it on you.

## The pads, per mode

- **PLAY — a pure drum machine**: all 16 pads fire the kit. A1-A8 are the
  8 kit sounds; B1-B8 are the SAME sounds one octave up (the Sampler's
  +12 pad mapping). No utility pads in PLAY — drumming can never erase a
  take.
- **EDIT — the arrangement row**: A1-A8 select the track the keys play;
  B1/B2 = undo/redo; B3-B8 = chord memory slots: press a slot that holds
  a chord to sound it on the selected track; to capture, hold the slot
  pad, play keys, release the pad (releasing the keys first is fine).
  The controller mirror shows a filled dot on every slot that holds a
  chord — an empty slot is silent by design.
- **KIT — the pack browser**: every pad selects one of the 16 built-in
  packs (A1-A8 = packs 1-8, B1-B8 = packs 9-16) and immediately auditions
  that pad's voice on the new kit, so switching packs is audible from the
  pads alone. The pack dropdown under the knobs shows what's loaded and
  switches packs from the mouse too.
- **Undo/redo everywhere**: EDIT pads B1/B2, or **Ctrl+Z / Ctrl+Shift+Z**
  on the keyboard. Undo removes the last recorded take; redo restores it.

## Sound

- **Four tracks — the keys always play the SELECTED track** (see
  *Keyboard routing* below):
  - **Synth tracks** — subtractive synth (2 osc + unison, ADSR, SVF
    filter, 2 routable LFOs, mono-legato glide, velocity→filter); the
    + menu creates them from the ten curated presets.
  - **Drums** — the drum kit (one shared kit); keyboard notes map into
    the pads, so any octave plays drum sounds. Drums run through their
    own FX chain too (glue EQ/delay work great here).
- **Every track has its own insert FX chain** (see *FX chain* below),
  processed live. The chain is also applied on WAV export, so what you
  hear is what you get.
- Knobs (PLAY mode): 1 volume · 2 pan · 3 send (reverb) · 4 macro ·
  5 filter cutoff · 6 resonance · 7 attack · 8 release — they act on the
  SELECTED track. The on-screen mirror knobs do the same (drag / wheel /
  double-click reset).
- Knobs (EDIT mode — the FX page): 1 fx select · 2 fx on/off (a click on
  the on-screen knob toggles; a drag on hardware sweeps) · 3 fx param A ·
  4 fx param B · 5 tempo · 6 loop bars · 7 quantize · 8 master.
  Parameters A/B depend on the selected effect (see *FX chain*).
- Knobs (KIT mode — the sound-design page): 1 velocity curve (4 zones) ·
  2 kit gain · 3 last-pad tuning (±12 st) · 4 last-pad decay ·
  8 master. "Last pad" = the last pad you hit in PLAY (or the one a KIT
  pack audition played).

## FX chain

Every track carries a 9-block insert chain (signal order):

| # | Effect | On means | Param A | Param B |
|---|--------|----------|---------|---------|
| 1 | **EQ** | any band ≠ 0 dB | low shelf ±18 dB | high shelf ±18 dB |
| 2 | **Comp** | switch | threshold | ratio 1..20 |
| 3 | **Crush** | switch | bit depth 2..16 | downsample 1..16 |
| 4 | **Drive** | drive > 1 | amount 1..8 | shape (soft/hard/fold) |
| 5 | **Chorus** | mix > 0 | rate | mix |
| 6 | **Flanger** | mix > 0 | rate | mix |
| 7 | **Delay** | mix > 0 | beats (1/16..1, snapped) | mix |
| 8 | **Trem** | depth > 0 | rate | depth (1 = autopan) |
| 9 | **Verb** | mix > 0 | decay | mix |

- The delay is tempo-synced: it follows the transport BPM live.
- **Knob 3 in PLAY mode (send) is the reverb amount** — it sets Verb mix
  on the selected track.
- The chain defaults to bypass; a default FX setup is bit-identical to no
  FX (the offline export determinism depends on this).
- The **FX panel** shows the same state: click a chip to select, drag the
  A/B lanes to set, pick a preset from the dropdown to load a whole
  curated sound.

## Presets

Ten curated {synth patch + FX chain} presets for the keys tracks — pick
them in the FX panel's preset dropdown (they apply to the SELECTED track;
manual knob edits mark it *(custom)*; the Drums track uses packs instead,
so the dropdown disables itself there):

| Preset | Sound |
|--------|-------|
| Default Saw | The shipped Keys A sound — everything bypass |
| Acid Bass | Mono glide saw, resonant filter, tanh drive + short delay |
| Warm Pad | Detuned 3-voice saw, slow attack, chorus + long reverb |
| Pluck Keys | Fast triangle/square pluck, velocity→filter, 1/8 delay |
| Square Lead | Mono glide lead with vibrato LFO + 1/4 delay |
| Sub Bass | Sine sub with mono glide, glued by the compressor |
| Dreamy Keys | Soft 2-voice keys, chorus + flanger + long reverb |
| Stabs | Short chord stabs, compressed, echo-filled 1/8 delay |
| Lo-Fi Keys | Dark EQ, 8-bit crush, drive, slow tremolo |
| Fly Lead | Detuned octave lead, 1/8 echo feed + reverb |

Any manual knob edit marks the track as custom (no preset lit).

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

### Keys: once vs repeat

A single key press sounds **once**. If a held key "plays itself" — rapid
repeats of the same note — the MPK mini's onboard **arpeggiator** is
engaged: it re-fires the held key at its own rate, and Ghost Story can only
shape that after the fact.

- **ONCE** (default, the `keys` chip in the controller mirror): a held key
  sounds exactly once; hardware re-fires are suppressed until the key is
  released. A stale mark self-expires after 5 s, so a device that unlatches
  via all-notes-off cannot leave a key permanently silent.
- **REPEAT**: every re-fire passes through — for playing the hardware arp
  or latch deliberately.

The chip is in the controller-mirror's status column (under the mode
strip); the toggle is also addressable as the `keys.play_once` action for
future hardware binding.

Device-side checklist when keys misbehave (knobs/pads keep working):

1. **ARP ON/OFF button lit?** Press it to disengage — an engaged arp
   captures the keybed (repeats, or silence if it waits on a clock).
2. Ghost Story does **not** send MIDI clock, so an arp set to external sync
   never fires: keys look dead while the keybed is actually captured.
3. Octave buttons only shift the key range — notes still arrive, just
   transposed; if a key sounds nothing at all, suspect the arp first.

## Pad packs

The pads ship with sixteen built-in packs — same eight pad slots, different
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
| 8 | Acoustic | Softer organic kit: rounded clickless kick, brushy snare, muted hats |
| 9 | Linn 80s | Dry 80s drum machine: clicky tight kick, gated hats, slap clap |
| 10 | DnB | Drum & bass: fast subby kick, crisp snare with body, razor hats |
| 11 | Dub | Deep and slow: long dark kick, fat tom, splashy open hat |
| 12 | Ambient | Soft muted everything — a kit that sits behind pads and keys |
| 13 | Electro | 808/909 hybrid: hard-click kick, metallic hats, tight clap |
| 14 | Garage | UK garage: mid punchy kick, sharp snare, tight swung hats |
| 15 | Breaks | Funk breaks: punchy kick, fat mid snare, crisp hats, present clap |

Switch packs from the UI (pad-pack control) or programmatically via
`PlayEngine::set_pad_pack()` / `cycle_pad_pack()`; switching is instant and
safe while playing. Your own WAVs can still replace individual pads of the
active pack (`load_override`) — and the mic panel can record one for you
live (see below).

## Microphone

Ghost Story has a live-input channel: whatever your system's default capture
device delivers (PipeWire capture stream, JACK input ports, or an ALSA
capture fallback) lands in the **mic panel** right of the arrangement
canvas.

- **Status dot** — green while frames are actually arriving; dim means no
  capture device opened (or input went silent). The resolved path is
  logged at startup (`pipewire capture stream created…` / `jack capture:` /
  `alsa capture open:`).
- **MONITOR** — mix the mic into the master output live, so you can sing,
  rap or play an instrument **over the beat while it runs**. Default OFF —
  an open mic through speakers feeds back; turn it on with headphones.
  (This line used to claim "Default ON" — the code always shipped OFF;
  the doc lied.) The mic follows the master fader like every other source.
- **gain** — 0–200 % input gain, applied before monitoring and before a
  grab; the level bar shows the post-gain peak.
- **Grab to pad** — the sampler trick: pick a duration (1 s / 2 s / 4 s),
  pick the target pad **P1–P8** (bank A, notes 36–43), hit **GRAB** — the
  last N seconds of mic audio are peak-normalized and loaded as that pad's
  sample in the ACTIVE pack. Switch to PLAY and the pad plays your voice.
  Great for beatboxing a hat, sampling a "hey", or building a kit from
  anything the room gives you. The grab reports its result in the panel
  (and the pad pulses in the controller mirror).
  - Grabs land on the active pack only — switching packs reverts to the
    built-in sounds (same as `load_override`).
  - Latency is one audio buffer (~10 ms at 512 frames): the monitor path
    is comfortable; play rhythmically against the beat with headphones.

## Export

```sh
ghost_story export --project-dir ~/.local/share/ghost_story/projects/default \
    --smf song.mid          # Standard MIDI file (type 1)
Ghost Story export --project-dir ... --wav-dir stems/   # stems + master WAV
```

The master mix equals the sum of stems (tested to 1e-6).

## Troubleshooting

- **No sound**: `ghost_story doctor` → audio backend line. PipeWire preferred;
  JACK and ALSA fallbacks are attempted automatically in that order.
- **Mic panel shows "no input"**: check that the backend log line at
  startup mentions capture (PipeWire creates an `ghost_story-mic` stream, JACK
  registers `ghost_story:in_L/in_R`, ALSA opens a capture PCM). Under JACK, a
  patchbay may have left the input ports unconnected — route a source to
  them manually. Under ALSA, capture needs a full-duplex-capable device;
  if `default` refuses, playback still works and the mic stays dim.
- **After a crash**: reports are in `~/.local/share/ghost_story/crashes/`; the
  next launch tells you. Run `ghost_story repair`, then `ghost_story doctor`.
- **If the knobs do nothing**: Ghost Story recognizes all three stock MPK knob
  CC layouts automatically — CC 1–8, CC 21–28 and CC 70–77 map to knobs
  1–8 by position. If your unit was reprogrammed to something else, the
  log prints `unmapped CC <n>=…` once per unknown CC; run
  `ghost_story calibrate` (writes `profile.toml` with your real knob CCs) or
  add `knobs.N.cc = <cc>` lines to a `profile.toml` yourself. Ghost Story
  loads it from the project directory first (`<project>/profile.toml`),
  else `$XDG_CONFIG_HOME/ghost_story/profile.toml`, else
  `~/.config/ghost_story/profile.toml`; the resolved knob CC map is logged at
  startup either way.
- **Privacy**: telemetry is local-only; `GHOST_STORY_TELEMETRY=0` disables it
  entirely.

## Log shipping & Kibana (development observability)

Every interactive run (`play`, and the `selftest-record` gate driver) appends
structured JSONL to
`~/.local/share/ghost_story/logs/app.jsonl` — one line per entry:

```json
{"ts_ns":1794321617132164672,"@timestamp":"2026-11-10T14:40:17.132Z","run":"4f3393b9","lvl":"INFO","tid":1034578977308104234,"th":"audio","sub":"app","msg":"..."}
```

- `@timestamp` is wall-clock ISO-8601 UTC (what Kibana uses as the document
  timestamp); `ts_ns` is the same instant in nanoseconds.
- `run` is this process run's 8-hex id — group one session's lines in
  Kibana, or correlate log ↔ telemetry ↔ crash report of the same run.
- `th` is the logging thread's label (`main`, `audio`, `midi`,
  `watchdog`, `alsa-writer`, `alsa-cap`, `log`); `sub` is the subsystem.
- The file rotates at 32 MiB (previous file kept as `app.jsonl.1`).
- Override with `ghost_story play --log-file PATH`, or `--no-log-file` to
  stay stderr-only; `GHOST_STORY_LOG_FILE=0` (or a path) does the same via
  the environment.

Three streams ship to the stack: the app log (`logs-ghost_story-app`), the
watchdog's `incidents.jsonl` (`logs-ghost_story-incidents`), and the local
telemetry `sessions.jsonl` (`logs-ghost_story-telemetry` — session_end
docs carry `xruns`, `max_callback_us`, `midi_events`, `duration_s`, so
those chart directly).

The stack in `deploy/elk/` ships these logs into Elasticsearch and makes
them searchable in Kibana, plus a "many errors" alert:

```sh
make elk-up      # Elasticsearch + Kibana + Filebeat (dev-only, 127.0.0.1)
make elk-setup   # index template, 30-day retention, data view, alert rule
make elk-smoke   # end-to-end gate: synthetic entries → indexed → searchable
make elk-down
```

Then open <http://127.0.0.1:5601> → **Discover**, data view
`logs-ghost_story`. Useful KQL: `lvl: ERROR`, `sub: audio and th: audio`,
`msg: "dropped"`, `kind: session_end and xruns > 0`, `"xrun"`.

**Alert**: `Ghost Story: many errors` fires when **more than 10 error
entries land within 5 minutes** — an error counts as `lvl: ERROR/CRIT`
*or* a log line mentioning "dropped" (note-drop storms from the synth
queue are WARN-level but audible, so they count too; a clean shutdown
logs nothing). Checked every minute; re-notifies only after the alert
clears — an ongoing error storm is reported once, a new burst
re-notifies. Tune or add a webhook — Slack/Discord/ntfy incoming-webhook
URLs all work — via env before `make elk-setup`:

| Env | Default | Meaning |
|---|---|---|
| `ERROR_THRESHOLD` | `10` | Alert above this many ERROR/CRIT entries |
| `ERROR_WINDOW_MIN` | `5` | Sliding window, minutes |
| `WEBHOOK_URL` | *(unset)* | Alert delivery; unset = Kibana server log |
| `ES_URL` / `KIBANA_URL` | `127.0.0.1:9200` / `:5601` | Point setup/smoke at a remote cluster |

**Webhook licensing**: webhook connectors are an Elastic Gold+/trial
feature. Without one, `elk_setup.sh` says so and falls back to the
server-log connector (alerts land in `docker compose logs kibana`). The
free 30-day trial enables webhooks:
`curl -XPOST http://127.0.0.1:9210/_license/start_trial?acknowledge=true`
(the dev stack on this machine already runs that trial). Rerunning
`make elk-setup` with a changed `WEBHOOK_URL` updates the stored URL in
place.

**Privacy**: the log file is local-only, like telemetry — nothing leaves
the machine until you start the stack and set a `WEBHOOK_URL`. Disable file
logging entirely with `--no-log-file` / `GHOST_STORY_LOG_FILE=0`.
