// addu94 — src/ui/controller_mirror.hpp
//
// ControllerMirror (spec §9): custom-painted hardware mirror of the MPK
// Mini — 8 knobs (arcs + action labels), 16 pads (2 banks of 8, active bank
// lit, hits decay over 150 ms), and the mode / pad-bank / octave status
// strip. Mode buttons are clickable (drives mapping().set_mode).
//
// Plain widget (no Q_OBJECT): repaints are driven by MainWindow's 30 Hz
// timer via refresh(); hits arrive via pulse_pad() from the same drain.
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>
#include <array>
#include <cstdint>

class QMouseEvent;
class QPainter;
class QPaintEvent;
class QRectF;

namespace addu94::ui {

class ControllerMirror : public QWidget {
public:
    explicit ControllerMirror(app::PlayEngine& engine,
                              QWidget* parent = nullptr);

    /// Pad hit feedback: absolute index 0..15 (bank A = 0..7 notes 36..43,
    /// bank B = 8..15 notes 44..51). Glow decays over 150 ms in paint.
    void pulse_pad(int pad_index);

    /// UI-tick hook: repaints while any pad glow or value change is live.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void paint_knobs(QPainter& p, QRectF const& area);
    void paint_pads(QPainter& p, QRectF const& area);
    void paint_status(QPainter& p, QRectF const& area);

    app::PlayEngine& engine_;
    std::array<std::int64_t, 16> pad_hit_ms_{};  // steady-clock ms of last hit
    std::int64_t now_ms_{0};
    QRectF mode_rects_[3]{};  // cached by paint, hit-tested on click
};

}  // namespace addu94::ui
