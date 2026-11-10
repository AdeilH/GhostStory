# Changelog

All notable changes. Format: Keep-a-Changelog-ish; dates UTC.

## [Unreleased]

### Fixed — every take keeps its own block, dead blocks die with their track

- **A new take no longer steals the previous take's block** (user:
  "when i record an instrument it removes the old recorded box — [rec 1]
  [rec 2] -> It removes rec 1 we want both and many instruments can
  interleave each other recording that's the whole part"). The take
  placement used to REPLACE the track's one clip window on every punch-in:
  recording take 2 at a later playhead position moved the whole clip (and
  its notes' playback spot) to the new take — rec 1's block vanished from
  where it was played. Now every take records its own span as a `TakeBox`
  on the clip: **the canvas draws one block per take** ([rec 1] [rec 2]
  both visible, interleaved across instruments), the clip's loop window
  only ever EXTENDS to cover all takes (a take punched before the window
  shifts stored events right, preserving every absolute position —
  playback semantics unchanged: the union window loops as before), and
  an overdub punched into an existing take merges into its box instead of
  stacking a twin block. Recording different instruments (tracks) keeps
  both takes — that path already worked and is now regression-tested.
- **Per-take editing**: right-click a block → **Clear this take** (the
  track and its other takes survive — the old right-click wiped the whole
  track's recording); edge-drag/double-click resize THAT take's box
  (`set_box_bars`; events past the trimmed end go with it); dragging a
  block moves the track's whole take-group (window + boxes ride
  together); undo/redo carries the box (an undo of a merged take keeps
  the earlier take's span); take boxes persist in project files
  (additive `boxes` array — older files load fine and render as the
  single whole-window block). A take armed but never played leaves NO
  ghost block: the transport places the live REC box before the first
  note, and stop/punch-out prunes noteless boxes.
- **Removing instruments leaves no dead blocks on the canvas** (user:
  "i have removed all instruments and this shows up" — the screenshot's
  ghost "Acid Bass" block over "NO INSTRUMENTS YET"). The arrangement
  view kept a per-track block vector forever: removed tracks' blocks were
  never pruned, and after a removal compacted the slots a surviving block
  wore the removed instrument's name. The canvas now prunes blocks of
  tracks that no longer exist every UI tick and reads the track name
  live from the engine at paint time — the canvas always mirrors engine
  truth.
- Tests: engine regressions for the two reports (interleaved instruments
  keep both takes; punch-in at a later position keeps rec 1's box at its
  own spot, two boxes, both audible through the union loop), noteless-take
  pruning; clip-level box tests (disjoint spans stay separate, overlap
  merges to the earliest take, grow-while-rolling, undo/redo carries the
  box, clear_take); project round-trip with boxes. ASan + TSan suites
  green.

### Added — logs land in Kibana, with a "many errors" alert

- **The app writes a log file now.** `play` (and the `selftest-record`
  gate driver) append structured JSONL to
  `~/.local/share/ghost_story/logs/app.jsonl` — the async logger always
  supported it, every call site just passed "" so nothing was ever on
  disk. Log timestamps switched from `steady_clock` (ns since boot —
  meaningless to anyone but the app) to wall clock, and each line now
  carries a ready-to-index `@timestamp` in ISO-8601 UTC alongside
  `ts_ns`. Every line also carries a per-run 8-hex `run` id (group one
  session in Kibana; correlate log ↔ telemetry ↔ crashes) and the
  logging thread's label `th` (`main`/`audio`/`midi`/`watchdog`/
  `alsa-writer`/`alsa-cap`/`log` — `Logger::set_thread_name()`, a POD
  thread-local, RT-safe; every thread that logs is named). The file
  rotates at 32 MiB (`app.jsonl.1`), parent directories are created,
  and shutdown warns about dropped entries only when the RT queue
  actually overflowed (clean runs stay at debug).
  `--log-file PATH` / `--no-log-file` / `GHOST_STORY_LOG_FILE` (0 =
  off) control it.
- **Telemetry joins the pipeline**: `sessions.jsonl` docs now carry
  `@timestamp` too, Filebeat ships them to
  `logs-ghost_story-telemetry`, and the index template maps
  `xruns`/`max_callback_us`/`midi_events`/`duration_s` as numbers —
  session quality charts without any app-side networking. The stack's
  data streams are filebeat-9 style (`logs-ghost_story-*`), so the
  index template is data-stream-enabled at priority 500 (above the
  built-in `logs-*-*`, whose keyword mappings would otherwise silently
  win) and the ILM policy rolls over weekly before the 30-day delete.
- **Dev observability stack** (`deploy/elk/` + `make elk-up`):
  Elasticsearch + Kibana + Filebeat in Docker, dev-only (security off,
  ports on 127.0.0.1). Filebeat tails `app.jsonl` and the watchdog's
  `incidents.jsonl` into `logs-ghost_story-*`; no networking entered
  the C++ app — the RT-path rule stands.
- **`scripts/elk_setup.sh`** (idempotent): retention policy, index
  template, Kibana data view `logs-ghost_story-*`, and the
  **"Ghost Story: many errors" alert rule** — more than
  `ERROR_THRESHOLD` (default 10) error entries within
  `ERROR_WINDOW_MIN` (default 5) minutes, checked every minute. An
  error entry is `lvl: ERROR/CRIT` **or** a line mentioning "dropped"
  (note-drop storms are WARN-level but audible — they alert; clean
  shutdowns don't). Delivered via a generic webhook connector
  (`WEBHOOK_URL` — Slack, Discord, ntfy… anything that accepts
  incoming webhooks), falling back to the Kibana server log when unset
  (webhook connectors are a Gold+/trial Elastic feature; on a basic
  license the script says so and falls back — the dev stack runs the
  free trial). Reruns update the template/rule/URL in place instead of
  duplicating. Verified end-to-end against a Kibana 9.2 stack: 12
  synthetic ERROR entries → alert fired → delivery with the rendered
  count and a Discover deep link (webhook POST and server-log both).
- **`scripts/elk_smoke.sh`** (`make elk-smoke`): end-to-end gate —
  appends synthetic schema-v2 entries, waits for Filebeat to index
  them, checks the ERROR sub-query and that the data view + rule exist.
  Manual gate, deliberately not part of `scripts/ci.sh` (Docker too
  heavy for the 8-stage gate). Tests: schema-v2 assertions and a new
  rotation test in `test_logger`.

### Added — dynamic tracks, play-to-record, placeable clips

- **Tracks are dynamic and start EMPTY** (user: "it's not necessary to
  have keys a keys b drum and bass — I will add them manually as
  needed"). The fixed 4-track engine became an 8-slot pool with a
  per-slot kind (Synth/Drums), name and transpose; the **+ add
  instrument** bar at the rail's bottom opens the dropdown of the ten
  synth presets (patch + FX chain preloaded — the preset IS the
  instrument template) plus the shared Drum Kit (one kit → one Drums
  track; the menu disables a second). **Right-click a track card to
  remove it.** EDIT-mode pads A1–A8 now select all 8 slots.
- **Recording starts when YOU play**: Record ARMS (nothing rolls); the
  first played note starts the transport and the take — punch-in while
  rolling unchanged. Stop also disarms. The metronome is purely
  **opt-in** now: play and record never force the click on (it was
  force-enabled on every Play — "annoying").
- **The playhead is clickable**: clicking the arrangement moves it, and
  play/record begin there ("record from specific times") via a new
  sample-exact position-set request; `Transport::start()` begins at the
  playhead instead of always bar 1.
- **Clips live where you drop them**: `Clip::start_tick` placement
  (honored by the RT player, the offline WAV/SMF exporters, the recorder
  window and the canvas) with **drag-to-move** (snap to bars, engine
  commit, new `ClipMove` journal op), **right-click → Clear recording**
  (placement survives; the existing `ClipClear` op journals it) and
  edge-resize/double-click-extend as before.
- Persistence caught up with reality: saves now carry real per-track
  name/kind/mixer state (the old save wrote four default rows and lost
  everything but clips), and loading adopts them; legacy fixed-4 files
  restore their exact old layout and byte-identical patches. Dormant
  machinery finally in use: the journal's AddTrack/RemoveTrack/
  SetTrackParam ops. Engine hygiene fixed en route: FX chains + synths +
  kit now learn the real sample rate at start (hardcoded 48000 in three
  places), all 8 FX chains pre-initialize before the audio thread (add/
  remove at runtime is allocation-free), and the master render hook
  installs at construction (it was start()-only, so any harness driving
  render callbacks without start() silently rendered nothing).
- Tests: first-note-start, playhead/placement (silent before the clip,
  audible at it), clear-keeps-track, add/remove-with-take-migration,
  metronome-never-forced; the audible-take gates now hear the TAKE (the
  old ones were accidentally passing on the forced click).

### Added — the loop machine actually plays: record → play → loop

- **Recorded takes play back now.** The live engine never scheduled clip
  events — a finished take was silent in the app while the metronome kept
  clicking, because only the offline WAV/MIDI exporter read the clip. A
  new RT clip sequencer (`music::ClipPlayer`: fixed-capacity snapshot
  behind a seqlock, no locks/alloc on the audio path) fires each track's
  recorded notes sample-clock-accurately against the transport, looping at
  the clip's own length. Pressing Stop releases still-sounding clip notes
  (note-offs, tails decay — a groovebox stop, not a hard mute).
- **Keys record in any octave.** The record gate silently accepted only
  notes 36–43 (C2–D#2, "full-key recording deferred with clip playback"):
  playing in a normal octave captured nothing. Now every note 0–127
  records at the sounding pitch (octave/scale applied like the live path),
  with the curve-applied velocity and the track's own channel, so playback
  lands on the instrument that was played. Keys on the Drums track record
  as their pad note; pads record into the Drums clip as before.
- **One clip per track (loop machine).** The engine held a single 4-bar
  clip for the whole project; the document format already supported
  per-track clip vectors. Keys now record into the SELECTED track's clip,
  pads into the Drums clip — layer instruments over each other. Undo/redo
  applies to the selected track's clip; journal + project round-trip are
  per-track (old single-clip projects load unchanged).
- **Arrangement canvas is real.** Each lane shows that track's actual
  clip (faint outline while empty); double-click a lane extends/creates
  the clip through the engine, a block's right edge resizes it (the old
  view-state drag moved nothing — an honest no-op now, and clip placement
  along a timeline is a ROADMAP milestone). An empty canvas paints the
  "press RECORD and play — any octave records" hint.
- The hardcoded 48000 Hz in the record tick math is gone (wrong quantize
  positions on any other sample rate); a record-side play-once guard
  stops sound-suppressed key re-fires from recording doubles.
- Tests: new `test_clip_player` (pure scheduling: windows, loop wrap,
  release, tempo republish, cap — plus end-to-end record→render gates:
  a recorded pad is AUDIBLE on playback, C5 records, stop quiets), and
  `test_recorder` extended (raw↔stored identity, pending binds its own
  clip, event cap). Docs: MANUAL "Making a loop" rewritten; the LOOP
  tooltip's inverted ∞ semantics and the mic-monitor "Default ON" doc lie
  are fixed.

### Changed — UI decongestion + a findable theme picker

- **THEME picker is visible now**: the health-strip combos carried no
  captions (a bare "Graphite" at the bottom edge read as a mystery
  setting), so they got painted **theme** / **velocity** captions, and the
  shared combo stylesheet gained explicit `::item` rules (min-height,
  padding, accent selection) so popup rows render as a plainly pickable
  list in every theme. One source for the QSS now lives in
  `theme::combo_stylesheet()`; the level meter no longer runs behind the
  theme combo (its lower third was occluded).
- **PACK chips → dropdown**: the 16 pad-pack chips wrapped to two rows of
  ~6pt text in the mirror — the densest element on screen. They are a
  labeled **pack** dropdown now (same `set_pad_pack` route, engine-truth
  sync each tick); KIT-mode pads remain the direct mid-performance path.
- **PRESET chips → dropdown**: the FX panel's 10 preset chips (also
  wrapping) are a **preset** dropdown with a `(custom)` row for manually
  edited tracks; on the Drums track it disables itself with a hint
  ("packs are the drum presets") instead of erroring on every pick like
  the chips did.
- **Breathing room**: the mirror's mode legend is one elided line (full
  map in the tooltip), the mic panel's P1–P8 grab targets are a 2×4 grid
  of real hit size with the explainer moved to the tooltip, track names
  got a wider text box (the 92px one elided half the names), and the
  health rows tightened to fund the captions.
- **Nothing clips at any size** (the "so many things are being cut off"
  report): track-card mixer lanes are centered in the card with a clamp —
  the fixed +42 offset used to push "vol 100%" past the card edge near
  the floor; mirror TRACK chips elide (dot when it fits) instead of
  clipping mid-word; the legend and OCT readout split the width by
  measured text, not a fixed 70/28; the nine FX slot chips greedy-wrap
  when the panel is narrow (they used to overflow past the panel edge);
  the FX description strip elides. The mirror (436px) and FX panel (228px)
  now declare the minimum widths their content needs — together with the
  health strip that is exactly the window floor, so "fits at the floor"
  holds by construction.
- **Two light-theme collisions fixed at real font sizes**: the health
  strip's "MASTER" caption and its "100%" value printed on top of each
  other (fixed 60px label box vs font-relative text — the value is
  right-aligned to the lane edge now, matching the panel's two-column
  rows), and the track cards' volume + pan wells sat 8px apart, reading as
  ONE slider whose handle looked parked at 75% when vol was 100% (now a
  clear 24px separation).
- **Honest window floor**: 960×560 was unreachable (the layout's true
  minimum is 584px, so Qt silently squeezed the bottom row until text
  collided) — the minimum is now the truthful **960×600**.

### Added

- **Live per-track FX chains** — the FxChain finally runs in the audio
  path (it was export-only). One insert chain per track (drums included),
  processed inside `render_into` before gain/pan; whole-struct FX params
  ride a new RT-safe SPSC queue (mirroring the synth ParamCmd pattern);
  the delay follows the transport BPM live. The old honest limitation is
  gone: **the PLAY-mode send knob (3) is now the track's reverb amount**.
- **Five new FX blocks + drive shapes** (EQ -> comp -> crush -> drive ->
  chorus -> flanger -> delay -> tremolo -> reverb): envelope-follower
  compressor (threshold/ratio/makeup), bitcrusher (bit quantize +
  sample-hold), hard-clip and foldback drive shapes alongside tanh,
  feedback flanger, tremolo/autopan (stereo-phase LFO), and a Schroeder
  reverb (4 combs + 2 allpass per channel, L/R spread) that also exists
  standalone (`Reverb`) for a future send bus. All default-bypass: a
  default FxParams is bit-identical to no FX (export determinism kept —
  the render suite passes unchanged). New tests: test_fx.cpp (13 tests:
  bypass identity, reverb tail/decay/mix-0, comp taming, crush quantize +
  hold, drive shapes differ + bounded, flanger vs feedback runaway,
  tremolo depth, everything-on stability, bit-identical determinism,
  RT-safe set_tempo echo relocation).
- **Eight more pad packs** (16 built-ins total): Acoustic, Linn 80s, DnB,
  Dub, Ambient, Electro, Garage, Breaks — the same deterministic recipes
  as new parameter tables. The controller-mirror pack chips now wrap
  (the old single row silently dropped chips past its width — 8 more
  packs would have become unclickable), and the pack-distinctness test
  generalizes to all 120 pairs.
- **Synth upgrades**: mono-legato mode (one voice per channel, envelope
  keeps running across retriggered notes) with exponential glide — the
  shipped Bass track now glides; velocity->filter (velocity opens the
  cutoff around a unity point at 0.5); a second routable LFO (both LFOs
  sum into shared modulation factors; Off adds no multiply, so single-LFO
  behavior is bit-identical to before). New tests: mono voice count,
  glide pitch ramp, velocity-filter brightness ratio, LFO2 tremolo.
- **Ten curated presets** ({synth patch + FX chain} pairs: Default Saw,
  Acid Bass, Warm Pad, Pluck Keys, Square Lead, Sub Bass, Dreamy Keys,
  Stabs, Lo-Fi Keys, Fly Lead) — applied via
  `PlayEngine::apply_preset()`; manual knob edits mark the track custom.
  `track_synth_params()`/`track_fx_params()`/`set_track_fx_params()`
  expose the app-side truth to the UI.
- **FX panel** (new UI widget, bottom row beside the mirror): 9 effect
  chips (lit = on, filled = selected), drag lanes for the selected
  effect's A/B parameters, and the preset chips. The lanes send the SAME
  mapping commands as the hardware, so UI and MPK knobs share one code
  path.
- **EDIT-mode knob page is now the FX page**: 1 fx select · 2 fx on/off ·
  3 fx param A · 4 fx param B · 5 tempo · 6 loop · 7 quantize · 8 master
  (Play mode unchanged). The old scrub coarse/fine bindings were
  stored-only no-ops (no transport position control exists) and click
  level already lives on the transport bar — both gave way; scrub remains
  for the future transport milestone. Four new actions (FxSelect/
  FxToggle/FxParamA/FxParamB); audit table, mirror labels and coverage
  test updated (zero dead controls still holds).
- **Live microphone input — monitor + grab-to-pad** (the "make music with
  your voice" feature). Three new lock-free pieces in
  `audio/capture_ring.hpp` house style: `CaptureRing` (backend → RT,
  drop-new-on-full) and `HistoryRing` (RT → app grab window,
  overwrite-oldest, ~11 s @48 kHz). Backends grew an optional capture
  path (BackendConfig::capture_ring): PipeWire creates a second
  `ghost_story-mic` capture stream on the same thread loop (single-producer
  guarantee preserved), JACK registers `in_L`/`in_R` input ports with
  best-effort `system:capture_*` auto-connect, ALSA opens a capture PCM
  in a reader thread (S16 fallback with conversion) — any failure
  downgrades to playback-only with a warning, never fatal. `render_into`
  pops the ring, rolls the post-gain history, and (monitor ON by default)
  mixes the mic into the master pre-fader — sing over the beat while it
  runs; latency is one buffer. **Grab to pad**: the last 1/2/4 s of mic
  audio are mono-downmixed, peak-normalized to 0.9 (boost capped 4x) and
  loaded as a bank-A pad zone via the new
  `DrumKit::load_override_data` → `Sampler::add_zone_data` (the
  documented zone-swap semantics unchanged). New **MicPanel** UI (right
  of the arrangement canvas): status dot (green = frames flowing, aged
  out when input stops), monitor toggle, gain 0–200 %, live level meter,
  duration + target-pad chips and the GRAB button; a grab pulses the pad
  in the mirror. Tests: test_capture.cpp (ring FIFO/drop-new,
  overwrite-oldest/read-last compaction, flow-controlled two-thread SPSC
  stress, PlayEngine mic monitor/level/history, grab refusals,
  DrumKit override → audible pad) — green under sanitizers.
- **The on-screen controls are real now** (the "laptop without the MPK"
  story):
  - **ControllerMirror knobs are interactive** — drag vertically, wheel
    nudge, double-click reset; every move emits the same
    `mapping::Command` the hardware knob would, through a hoisted
    single-source-of-truth `MappingEngine::knob_action()/knob_label()`
    (the old UI-side label table drifted from the action table once
    already). EDIT knob 2 (FxToggle) is click-to-toggle, not a drag.
    PLAY vol/pan/send and the master knob display sync from engine truth
    each tick (they used to sit at 0 while the mix ran at 100%).
  - **Pads are playable per mode** — PLAY drums (with the mirror pulse),
    EDIT bank A track select / B1-B2 undo-redo / B3-B8 chord slots with
    mouse-down/mouse-up press-release pairs, KIT pack select + audition.
  - **TrackStrips drive the mixer for real** — volume (now honestly
    0–100 % to match the engine's 0..1 gain; the fictional 150 % is gone)
    and pan drags call new per-index `set_track_volume/pan` setters (the
    `TrackVolume` command only reaches the SELECTED track — wrong target
    for a 4-row mixer), **M actually mutes** (new `track_mute_` atomics
    gating the RT mix loop; FX tails keep decaying), the **rec dot arms
    the loop recorder per track** (new `record_arm_` mask; nothing armed
    = everything records, shipped behavior preserved), and refresh()
    reads engine state so hardware edits stay in sync.
  - **FX panel toggle**: click a chip's dot corner (or right-click) to
    toggle that effect — the tooltip promised this since the FX panel
    shipped; the FxToggle action existed, the UI never sent it.
  - **HealthStrip**: the velocity-curve combo now drives the engine curve
    (it was view-state-only; `vel_curve_` became an atomic read by the
    MIDI thread per note-on), plus a master fader (drag → MasterVolume,
    100 % = unity) beside the level meter.
  - **Transport consistency**: the UI stop button and Space now route
    through the TransportStop command, so stopping from the screen also
    flushes recorder pending notes and saves the project (the raw
    kReqStop silently skipped both); play goes through TransportPlay and
    gets the same metronome-on default as hardware.
  - **KIT knobs are no longer all None** — new actions
    VelocityCurveSet (4 zones) / KitGain (sampler gain 0..2) / PadTune
    (±12 st) / PadDecay (0.1..4 exponential) on the last-hit pad; kit
    params ride a new RT SPSC queue (`kit_q_`) so a knob turn can never
    race a note_on read. Audit table stays green.
- **Real window sizing** (the window used to be pinned at ≥1280×720 —
  too big for 1366×768 laptops and tiling WMs). The floor is now a
  compact 960×560 and the window grows, shrinks and maximizes freely;
  **F11 toggles fullscreen** (Esc leaves it) no matter what the window
  manager offers. The chosen size and maximized/fullscreen state are
  remembered across runs via QSettings
  (`~/.config/ghost_story/ghost_story.ini`); GS_SHOT screenshot runs
  never touch the saved geometry.
- **Four themes** — Graphite (new default: neutral charcoal, restrained
  steel accent), Midnight (the original launch look), Slate (blue-gray +
  calm teal), Paper (light: warm paper, ink text, terracotta accent).
  The THEME combo lives in the health strip above the master fader; the
  whole instrument repaints on the next 30 Hz frame and the choice is
  persisted via QSettings. Every surface tint the widgets used to
  hardcode (knob bodies, pad fills, cards, canvas floor, transport
  buttons) is now derived from the palette, so all four themes keep
  their own micro-contrast; `GS_SHOT_THEME=<name>` pins a theme for
  screenshot runs. The app also prefers an installed humanist sans
  (Inter → Noto Sans → Open Sans → DejaVu Sans) and the label tracking
  relaxed a touch — small typography decisions, less machine-generated
  feel.

### Changed
- **Mic monitor now defaults OFF** (was ON). An open mic mixed straight
  into the master through speakers feeds back — howl and scratch — and
  every DAW ships input monitoring opt-in for exactly that reason. Turn
  it on in the MIC panel when you want live monitoring (headphones
  recommended). Grab-to-pad is unaffected: it reads the capture history,
  not the monitor mix.
- **Renamed the project to Ghost Story.** The binary `addu94` is now
  `ghost_story`, targets `addu94_core`/`addu94_ui` are
  `ghost_story_core`/`ghost_story_ui`, CMake options `ADDU94_*` are
  `GHOST_STORY_*`, and the short-form test-framework internals (`a94_*`,
  `A94_CONCAT`) are `gs_*`/`GS_*`. Data and config directories moved too:
  `~/.local/share/addu94/` and `~/.config/addu94/` are now
  `.../ghost_story/` — existing user data (projects, profile.toml,
  manifests, crash/telemetry reports) is NOT migrated; copy it over or
  re-run `ghost_story probe`/`ghost_story calibrate`. The UI-screenshot
  env vars `A94_SHOT`/`A94_SHOT_PLAY` are now `GS_SHOT`/`GS_SHOT_PLAY`.
  Packaging follows: man page `docs/addu94.1` → `docs/ghost_story.1`,
  desktop file → `packaging/ghost_story.desktop` (`Name=Ghost Story`),
  `pkgname=ghost_story`; window title, `QApplication` name and the
  onboarding banner read "Ghost Story".

### Fixed — user report, round 2: "bank B not working" + "no other pack on
pad working"
- **Scratchy output noise, root-caused and fixed** (user report: "weird
  scratchy noise when running"). Two compounding bugs in the live-input
  path: (1) the PipeWire capture stream ignored the SPA buffer
  contract — valid samples live at `data + chunk->offset`, but the
  handler read from `data[0]`, so whenever the graph handed back a
  non-zero offset the "audio" was stale ring memory: periodic bursts of
  garbage every ~10.7 ms buffer, i.e. exactly the scratch, fed straight
  into the master by the default-on monitor. The handler now honors the
  offset and clamps to what the plane actually holds. (2) Mono sources
  (laptop mics) negotiated one channel, and the old
  both-planes-required check dropped EVERY capture cycle — capture was
  silently dead on mono inputs; a mono source now duplicates into both
  ring planes. Bench stays clean (0 xruns, p99 8 µs — the engine was
  never the problem; the signal was).
- **Root cause, stated honestly**: v0.1.3 gave bank B to utilities and
  chord slots, which made a drummer's bank B SILENT — the pads' primary
  job in PLAY is making sound, and no amount of log lines changes that.
  Pack switching, meanwhile, existed only as UI chips: no hardware path
  at all. Round 1's fixes (logs, feedback, EDIT row) treated symptoms;
  this one changes the semantics.
- **PLAY is now a pure drum machine — all 16 pads fire the kit**: 36..43
  the 8 kit sounds, 44..51 the SAME sounds one octave up (the Sampler's
  +12 pad mapping, which the v0.1.3 redesign orphaned, is live again).
  Bank B wears the same per-voice hues as bank A (B4 reads as "A4, octave
  up"). No utility pads in PLAY, so drumming can never reach Undo — a
  strictly stronger guarantee than v0.1.3's "no accidental undo chord".
- **KIT mode (the old STEP) is the pack browser**: every pad selects one
  of the 16 built-in packs (A = packs 1-8, B = packs 9-16) and
  immediately auditions that pad's voice on the new kit — switching packs
  is audible from the pads alone, no mouse needed. The mirror's mode chip
  relabeled STEP → KIT (the enum stays `Mode::Step`; if a pattern editor
  ever lands as a pad mode the label flips back). Pack chips also log
  failures instead of swallowing the error string.
- **EDIT keeps the arrangement row**: bank A track select; B1/B2 undo/
  redo; B3-B8 chord slots (capture via hold + keys — round 1's
  gesture-robust snapshot and filled-dot mirror state carry over).
  Chords moved out of PLAY with everything else; they capture and sound
  on the selected track in EDIT.
- **Undo/redo on the keyboard too**: Ctrl+Z / Ctrl+Shift+Z (UI-originated
  commands pulse the mirror pads directly — they don't ride
  drain_app_events).
- **Mode-aware pad routing**: the RT drum gate reads a new `mode_rt_`
  atomic (written only by the new `PlayEngine::set_mode`, which ALL mode
  switches — commands and mirror clicks — now go through; the old direct
  `mapping().set_mode` route would have left the gate stale). EDIT/KIT
  pad presses no longer fire stray drum hits, so chord capture and pack
  selection stay quiet. Keys are not gated — they play the selected
  track in every mode, unchanged.
- **Recording windows**: channel-9 pads record only in PLAY (36..51, so
  +12 hits record at their raw note); keys record in every mode
  (bottom-octave window 36..43 — full-key recording stays deferred with
  clip playback). The round-1 off/on window mismatch is gone with it.
- **`PadBankToggle` removed** — no emitter ever existed and the bank is
  note-range-determined by design; the mirror's dimmed "other bank" went
  with it (every pad is live in every mode now).
- `PadPackSelect` is a new action (`pads.pack`, param = pack 0..15).
  Audit table rewritten in lockstep with `map_note` (third time's the
  charm — the lockstep comment now names all three drifts).
- Tests updated: PLAY bank B = DrumPad 8..15; undo/redo EDIT-only with
  PLAY-44 regression; chord slots in EDIT; KIT pads = PadPackSelect 0..15;
  the drumming-never-undoes regression now covers bank B.

### Fixed — user report: "BANK B doesn't work at all" (round 1)
- **B2/Redo was a silent no-op**: the mapping emitted `Action::Redo` (and
  the mapping test even asserted it) but `PlayEngine::command()` had no
  case for it, and `Clip` had no redo stack — undo was destructive with no
  way back. Clip now keeps a per-take redo stack (multi-level:
  undo/undo/redo/redo walks the take history), `Recorder::begin_take()`
  invalidates it (the fresh take reuses the stashed take index), and the
  engine case logs `clip: redid take N` / `clip: nothing to redo` — undo/
  redo are otherwise silent, and "pad did nothing" has to be answerable
  from the log.
- **B3–B8 chord slots looked dead**: an empty slot press is silent by
  design, but nothing said so and nothing showed what a slot held. Empty
  slots now log one how-to-capture line per slot per process (same
  courtesy as unmapped CCs), and the controller mirror paints a filled dot
  on every slot that holds a chord. Capture is also gesture-robust now:
  the old rule captured only keys still held at pad release, so releasing
  the keys first (the natural gesture order) silently skipped the
  capture — slots now snapshot (grow-only) every key played during the
  hold, so quick stabs and keys-released-first both capture.
- **EDIT-mode bank B was six dead pads**: the coverage table promised a
  transport row for 46..51 that `map_note` never emitted (and half the
  promised actions had no engine cases). EDIT bank B is now real, all
  handled actions: B3 metronome, B4 arp latch, B5 loop on/off (restores
  the last loop length; Transport wraps position live), B6 punch-in
  (record toggle), B7 play, B8 stop — press only. Count-in was the old
  table's promise, but Transport has no count-in engine, so the pad got
  the real arp toggle instead of a stored-only no-op; `count_in_bars`
  stays documented as consumer-less until the transport milestone.
- **The coverage audit had drifted**: it listed chord slots for bank A in
  PLAY (actual: DrumPad) and a Punch/ClipStop row nothing emitted. Table
  now matches `map_note` exactly, with a lockstep comment on both sides.
- **Sampler**: an override WAV loaded at notes 44..51 (`load_override`
  accepts 36..51) was unreachable — `note_on` remapped 44..51 onto the
  36..43 zones before checking for an exact zone, so `has_zone(44)` said
  yes while the pad stayed silent. Exact-zone lookup now wins first, and
  the +12 fallback for bank-B notes is regression-tested. The stale
  "bank B = same pads +12" claims in the DrumKit/Sampler headers are
  corrected (bank B is utilities/chords app-side since v0.1.3; the +12
  mapping stays for direct hardware/SFZ use).
- **Recorder gate asymmetry**: note-offs recorded in the 36..51 window
  while note-ons only recorded 36..43 — bank-B pad releases hit the
  recorder as orphan offs with no matching on. Both gates are 36..43 now.
- New tests: clip undo/redo roundtrip + multi-level + new-take
  invalidation (test_recorder), EDIT bank-B transport row + press-only
  releases (test_mapping), override-zone reachability at note 44 and the
  +12 fallback (test_sampler).

### Fixed — user reports: "knobs do nothing", "keys stick on one output",
"can't work out how to use different packs with the pads"

- **Knobs**: the mapping engine now recognizes ALL three stock MPK knob CC
  layouts by position — CC 1–8, CC 21–28 and CC 70–77 (it only knew 1–8).
  `knobs.N.cc` profile entries still override. Unmapped CCs log one line
  per CC number per process pointing at `ghost_story calibrate` /
  `profile.toml`.
- **Knobs do things now**: PlayEngine implements every knob action —
  TrackVolume/TrackPan as per-track render gain (equal-power pan),
  FilterCutoff/FilterReso/EnvAttack/EnvRelease/InstrumentMacro1 pushed to
  the selected track's synth through an RT-safe parameter queue,
  MasterVolume (0..1.5), ClickLevel (metronome gain), QuantizeGridSet
  (off / 1/8 / 1/16 / 1/32 zones — Grid gained ThirtySecond). Honest
  limitations, documented: TrackSend is stored for the FX-send milestone
  (fx.hpp has no send bus yet), ScrubCoarse/Fine are stored until
  Transport gains a position-set control.
- **Profile loading**: `profile.toml` (from `ghost_story calibrate`) is now
  actually loaded — project dir first, then `$XDG_CONFIG_HOME/ghost_story/`,
  then `~/.config/ghost_story/`; the resolved knob CC map is logged at startup.
- **Keyboard routing**: 4 tracks, each its own sound — Keys A (default
  patch), Keys B (square lead), Drums (keyboard notes map to pads mod-8,
  any octave), Bass (−12, mono-ish sub). The keys always play the SELECTED
  track (arp + chord slots follow). New API: `track_count()`,
  `track_name()`, `track_role()`, `selected_track()`, `select_track()`,
  `track_volume()`, `track_pan()`, `track_send()`, `TrackRole`.
- **Pad packs**: DrumKit now ships 4 built-in packs — "MPC Classic"
  (previous recipes, bit-identical), "808", "Lo-Fi", "Techno" — the same
  DSP with different parameter tables, all synthesized at init. Pack
  switching is a single atomic index (race-free against the audio
  callback; zone-table mutation while rendering was not). New API:
  `pack_count()`, `pack_name()`, `load_pack()`, `current_pack()`,
  `PlayEngine::pad_packs()/set_pad_pack()/cycle_pad_pack()`.
- **Synth**: the per-voice extension state moved from a process global
  into the Synth instance — running 3 synths concurrently (the new track
  model) would otherwise clobber unison/filter state across tracks.
- **VoiceCmd queue** is now MPMC: live keys (MIDI thread) and arp/chord
  strikes (app thread) are genuinely two producers — the old SPSC could
  corrupt under that overlap.
- New tests: knob alias sets / override / unmapped-CC (test_mapping),
  pack names / switching / per-pack audibility / determinism
  (test_sampler). 20/20 suites green.

## [0.1.3] — 2026-11-04

### Fixed — pad semantics (user report: "pad 1 and pad 2 have same response")
- Root cause: A1+A2 was the Undo chord — drumming the two pads in sequence
  fired Undo (erasing takes!), and the ChordSlot/ClipLaunch actions pads
  were mapped to were never implemented, so pads had no distinct response.
- New PLAY-mode pad semantics, all immediate and distinct:
  - **A1–A8 (notes 36–43): drum pads** — 8 distinct kit sounds, instant.
  - **B1/B2 (44/45): undo / redo** — instant, dedicated pads.
  - **B3–B8 (46–51): chord memory slots** — tap plays the stored chord
    (note-ons on press, note-offs on release); hold ≥400 ms while playing
    keys captures the current keys into the slot.
  - EDIT mode: A1–A8 selects tracks (press, was release).
- No two-pad chords anywhere: drumming can never erase a take.
- Mirror legend line ("A drums · B1/B2 undo/redo · B3-B8 chords
  (hold+keys=capture)") so the functions are discoverable on-screen; each
  pad pulses its own action in the mirror.
- Coverage audit updated (all controls still bound — zero dead controls).
- New test suite test_mapping.cpp (7 tests) incl. the exact regression:
  drumming A1→A2 produces two distinct DrumPad hits and never Undo.
- 20 test suites green; make ci ALL GATES GREEN.

## [0.1.2] — 2026-11-04

### Added — UI modernization ("one screen = the instrument")
- Single-screen layout replacing the tabbed UI: transport bar (drawn
  play/stop/rec/metronome icons, shortcut hints), left track rail with
  inline mute/arm/volume/pan per track, central arrangement canvas
  (section bands, per-track-hue pattern blocks, drag-to-move with bar
  snap, double-click to extend, glowing playhead + trail, loop shading),
  restyled controller mirror with segmented mode control, health strip
  with device LED, live xrun/callback readouts and an honest master
  level meter (RT-safe peak tap in PlayEngine).
- Musician interactions: Space = play/stop, R = record, M = metronome;
  drag BPM/loop values; tooltips everywhere.
- Permanent UI test aid: GS_SHOT=<path> grabs an offscreen screenshot
  (GS_SHOT_PLAY=1 starts transport for live-state captures).

### Fixed
- **Mix bug** (found via the UI meter debug): Sampler::render overwrote
  buffers, silently wiping the synth when drums played through PlayEngine
  — sampler now accumulates into caller-zeroed buffers (engine contract);
  verified synth+drums mix (kick+snare RMS over sustained synth).

### Verified
- 30-minute soak (spec §11, full duration): PASS — 0 xruns, 35,609 note
  events journaled and recovered, fuzz survived.
- UI verified through 6 offscreen screenshot iterations; 19 test suites;
  make ci ALL GATES GREEN.

## [0.1.0] — 2026-11-04

Milestones M0–M8 complete. Highlights across the release:

- **Instruments**: subtractive synth (2 osc + unison, ADSR, SVF, LFO),
  WAV sampler (zones, per-pad tuning/decay, choke groups), drum kit with
  synthesized CC0 fallbacks — playable with zero external files.
- **Workflow**: loop recording with quantize, take overdub, per-take undo;
  scale-lock; software arp (6 modes, octave stack); pattern clips +
  arrangement blocks/sections; crash-safe project persistence (WAL
  journal, per-append fsync, atomic saves, quarantine).
- **Native Qt6 UI** (dark, hardware-first): controller mirror, transport
  bar with live health readouts, Arrangement/Pattern/Mixer/Device/Health
  views; runs offscreen for CI.
- **Ops**: watchdog with structured incidents, crash handler + safe mode,
  local-only opt-out telemetry, doctor/repair/bench/probe/export CLI,
  MIDI fuzz harness (spec §7.5) and kill -9 recovery as CI gates.
- **Gates (all green at tag)**: 18 test suites; CI = build → ctest →
  ASan/UBSan → TSan → kill -9 recovery → MIDI fuzz → clang-tidy; live
  bench: p99 callback 6 µs (0.1 % of budget), 0 xruns / 5 min.
- **Live-verified on MPK mini 3**: probe/identity (mfg 0x47, fw 1.1.8),
  pads on ch 9, keys→synth→speakers, export SMF+WAV.

Deferred (tracked in ROADMAP.md): JACK/ALSA backend implementations
(interface shipped, PipeWire is the production path), FLAC export (WAV
shipped), hardware ARP/preset SysEx editing, on-hardware acceptance
checklist (§12 items 1–6 need the owner's hands on the device).
