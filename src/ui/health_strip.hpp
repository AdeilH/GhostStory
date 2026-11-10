// ghost_story — src/ui/health_strip.hpp
//
// HealthStrip: the right-hand panel of the mirror row (device/health tabs
// folded into ~224px). Custom-painted in the shared glass language: device
// LED (green with a soft ring when connected, red when not) + state,
// hairline-separated health rows (tiny tracked label left, colored value
// right — xruns, callback µs, backend, rate), a live master level meter —
// two thin vertical bars driven by PlayEngine::master_level() (peak
// |sample| per RT buffer) in recessed slots with a green→yellow→red
// gradient and 30 fps decay in refresh() — and a MASTER fader (drag →
// the same MasterVolume command the EDIT hardware knob emits, unity at
// 100%).
//
// The velocity-curve selector (formerly the Device tab) lives here as a
// small combo styled to the theme; it drives the engine curve the MIDI
// thread applies per note-on. The THEME picker sits below it: flips the
// process-global palette (custom-painted panels read it at paint time;
// MainWindow's tick re-styles the window/app palette when the generation
// moves) and persists the choice via QSettings.
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's timer.
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>
#include <QRectF>

class QComboBox;
class QMouseEvent;

namespace ghost_story::ui {

class HealthStrip : public QWidget {
public:
    explicit HealthStrip(app::PlayEngine& engine, QWidget* parent = nullptr);

    /// UI-tick hook: read stats + master level (with decay), repaint, and
    /// keep the velocity combo synced with the engine value.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    void set_master_from_pos(QPointF const& pos);
    void style_combos();  // rebuild combo stylesheets from the palette

    app::PlayEngine& engine_;
    QComboBox* velocity_curve_;
    QComboBox* theme_;
    int theme_generation_{-1};  // restyle when theme::generation() moves
    float level_{0.0f};  // decayed master level (0..1)
    QRectF master_rect_{};  // master fader lane, cached during paint
};

}  // namespace ghost_story::ui
