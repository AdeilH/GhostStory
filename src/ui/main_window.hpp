// addu94 — src/ui/main_window.hpp
//
// MainWindow (spec §9): dense, dark, hardware-first native shell.
//   top     — transport bar (play/stop/record, metronome, bpm, loop bars,
//             click level, position, xrun counter, device LED)
//   center  — QTabWidget: Arrangement / Pattern / Mixer / Device / Health
//   bottom  — ControllerMirror (always visible)
//
// Threading: the Qt thread only touches the documented thread-safe control
// plane — drain_app_events() at 30 Hz, transport_control() atomics,
// mapping() state, stats() and transport_position() snapshots. No Q_OBJECT:
// signals handled with lambdas, timer via timerEvent override.
#pragma once

#include "app/play_engine.hpp"
#include "ui/arrangement_view.hpp"
#include "ui/controller_mirror.hpp"
#include "ui/device_view.hpp"
#include "ui/health_view.hpp"
#include "ui/mixer_view.hpp"

#include <QMainWindow>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimerEvent;
class QVBoxLayout;

namespace addu94::ui {

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(app::PlayEngine& engine,
                        QWidget* parent = nullptr);

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void build_transport_bar(QVBoxLayout* root);
    void tick();

    app::PlayEngine& engine_;

    // Bottom mirror + tabs.
    ControllerMirror* mirror_;
    ArrangementView* arrangement_;
    MixerView* mixer_;
    DeviceView* device_view_;
    HealthView* health_view_;

    // Transport bar widgets.
    QPushButton* play_btn_;
    QPushButton* stop_btn_;
    QPushButton* rec_btn_;
    QPushButton* metro_btn_;
    QDoubleSpinBox* bpm_;
    QSpinBox* loop_bars_;
    QSlider* click_level_;
    QLabel* position_label_;
    QLabel* xrun_label_;
    QLabel* device_label_;
    int ui_timer_id_{0};
};

}  // namespace addu94::ui
