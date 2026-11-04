// addu94 — src/ui/health_strip.hpp
//
// HealthStrip: the right-hand panel of the mirror row (device/health tabs
// folded into ~224px). Custom-painted in the shared glass language: device
// LED (green with a soft ring when connected, red when not) + state,
// hairline-separated health rows (tiny tracked label left, colored value
// right — xruns, callback µs, backend, rate), and a live master level
// meter — two thin vertical bars driven by PlayEngine::master_level()
// (peak |sample| per RT buffer) in recessed slots with a green→yellow→red
// gradient and 30 fps decay in refresh().
//
// The velocity-curve selector (formerly the Device tab) lives here as a
// small combo styled to the theme (stored in-widget until M6 wires the
// synth-side curve).
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's timer.
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>

class QComboBox;

namespace addu94::ui {

class HealthStrip : public QWidget {
public:
    explicit HealthStrip(app::PlayEngine& engine, QWidget* parent = nullptr);

    /// UI-tick hook: read stats + master level (with decay), repaint.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    app::PlayEngine& engine_;
    QComboBox* velocity_curve_;
    float level_{0.0f};  // decayed master level (0..1)
};

}  // namespace addu94::ui
