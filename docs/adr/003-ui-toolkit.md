# ADR 003 — UI Toolkit: Qt6 Widgets

- **Status:** Accepted
- **Date:** 2026-11-03

## Context

Requirements: single window, dark dense "hardware-first" UI with a persistent
controller mirror, arrangement timeline, piano roll/step editor, mixer, device
and health views; Wayland + X11; 1280×720 minimum; must coexist with a
realtime audio thread without UI-induced dropouts.

## Decision

**Qt6 Widgets** (`Qt6::Widgets`), present on this machine. Justification vs
alternatives:

- **Qt6 Widgets** (chosen): mature Wayland/X11 support, custom-painted
  widgets (timeline, mirror, VU) are straightforward `paintEvent` work; no
  scene-graph overhead we don't need; ties into CMake cleanly.
- **GTK4:** good Wayland story but C API glues poorly with our C++26 core
  and its ecosystem pulls a second object model.
- **JUCE:** rejected — see ADR 002.
- **Dear ImGui + SDL/GL:** fastest to prototype but a11y, Wayland windowing
  and long-term maintainability suffer; also drags a GL context next to our
  audio thread.

## Rules

- The UI thread **never** touches MIDI or audio directly. All interaction is
  via the mapping layer's command queue and the lock-free mirror state
  (atomics / SPSC), so a blocked UI can stall nothing.
- UI is a thin observer over the project document; editing goes through
  commands (undo/redo lives in the project layer, not the UI).

## Consequences

- Qt linkage is introduced at M4 (controller mirror) — earlier milestones
  build and test headless; the app binary gains `--headless` mode permanently
  so CI runs without a display.
