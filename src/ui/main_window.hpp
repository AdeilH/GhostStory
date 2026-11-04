// addu94 — src/ui/main_window.hpp
//
// MainWindow — "one screen = the instrument" (no tabs):
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ TRANSPORT: ▶ ■ ● | BPM 120.0 | 4/4 | LOOP ∞ | click | pos │ 64px
//   ├─────────┬────────────────────────────────────────────────┤
//   │ TRACKS  │ ARRANGEMENT CANVAS (custom-painted)            │
//   │ 4 rows: │ ruler w/ sections, pattern blocks, playhead    │
//   │ mute/arm│                                                │
//   │ vol/pan │                                                │
//   ├─────────┴────────────────────────────────────────────────┤
//   │ CONTROLLER MIRROR (always visible)   |  health strip     │
//   └──────────────────────────────────────────────────────────┘
//
// Shortcuts: Space = play/stop toggle, R = record toggle, M = metronome.
//
// Threading: the Qt thread only touches the documented thread-safe control
// plane — drain_app_events() at 30 Hz, transport_control() atomics,
// mapping() state, stats()/master_level() and transport_position()
// snapshots. No Q_OBJECT: signals handled with lambdas, timer via
// timerEvent override.
#pragma once

#include "app/play_engine.hpp"
#include "ui/arrangement_view.hpp"
#include "ui/controller_mirror.hpp"
#include "ui/health_strip.hpp"
#include "ui/track_strip.hpp"

#include <QMainWindow>
#include <QRectF>

class QKeyEvent;
class QTimerEvent;

namespace addu94::ui {

class TransportBar;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(app::PlayEngine& engine,
                        QWidget* parent = nullptr);

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void tick();
    void toggle_play();
    void toggle_record();
    void toggle_metronome();

    app::PlayEngine& engine_;

    TransportBar* transport_;
    TrackStrips* tracks_;
    ArrangementView* arrangement_;
    ControllerMirror* mirror_;
    HealthStrip* health_;
    int ui_timer_id_{0};
    std::int64_t last_clip_len_{-1};  // clip length_ticks dedupe for block feed
};

}  // namespace addu94::ui
