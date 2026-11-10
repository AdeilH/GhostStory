// ghost_story — src/ui/main_window.hpp
//
// MainWindow — "one screen = the instrument" (no tabs):
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ TRANSPORT: ▶ ■ ● | BPM 120.0 | 4/4 | LOOP ∞ | click | pos │ 64px
//   ├─────────┬───────────────────────────────────────┬────────┤
//   │ TRACKS  │ ARRANGEMENT CANVAS (custom-painted)   │ MIC    │
//   │ 4 rows: │ ruler w/ sections, pattern blocks,    │ strip  │
//   │ mute/arm│ playhead                              │ (live  │
//   │ vol/pan │                                       │ input) │
//   ├─────────┴───────────────────────────────────────┴────────┤
//   │ CONTROLLER MIRROR      | FX + presets      | health      │
//   └──────────────────────────────────────────────────────────┘
//
// Shortcuts: Space = play/stop toggle, R = record toggle, M = metronome,
// F11 = fullscreen toggle (Esc leaves fullscreen).
//
// Window sizing: the floor is small (960×600 — the layout's true minimum)
// so the instrument shrinks with the screen; the chosen size / maximized /
// fullscreen state is remembered across runs via QSettings (skipped under
// GS_SHOT so test runs never touch the user's saved geometry).
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
#include "ui/fx_panel.hpp"
#include "ui/health_strip.hpp"
#include "ui/mic_panel.hpp"
#include "ui/track_strip.hpp"

#include <QMainWindow>
#include <QRectF>

class QKeyEvent;
class QTimerEvent;

namespace ghost_story::ui {

class TransportBar;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(app::PlayEngine& engine,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void timerEvent(QTimerEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void tick();
    void toggle_play();
    void toggle_record();
    void toggle_metronome();
    void restore_window_state();
    void save_window_state();
    void restyle();  // re-apply the active theme to window + app palette

    app::PlayEngine& engine_;

    TransportBar* transport_;
    TrackStrips* tracks_;
    ArrangementView* arrangement_;
    MicPanel* mic_panel_;
    ControllerMirror* mirror_;
    FxPanel* fx_panel_;
    HealthStrip* health_;
    int ui_timer_id_{0};
    int theme_generation_{0};         // restyle when theme::generation() moves
    bool geometry_restored_{false};   // first showEvent applied saved state
    bool start_fullscreen_{false};    // saved state asked for fullscreen
};

}  // namespace ghost_story::ui
