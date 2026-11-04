// addu94 — src/ui/track_strip.hpp
//
// TrackStrips: the left rail of the "one screen = the instrument" layout —
// four inline track rows, each with a per-track hue chip + name, mute (M)
// and arm (rec dot) toggles, a thin custom-painted volume bar (0..150%)
// with a draggable handle, and a mini pan indicator (draggable, -100..100).
//
// Clicking a row selects the track (engine.command TrackSelect) and
// highlights it. Volume/pan/mute/arm changes are reported through the
// track hook, which MainWindow wires to the log until real mixer state
// lands (values live in this widget for now).
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's 30 Hz
// timer (keeps the selected-row highlight in sync with hardware selects).
#pragma once

#include "app/play_engine.hpp"

#include <QColor>
#include <QWidget>
#include <array>
#include <functional>

class QMouseEvent;
class QPaintEvent;

namespace addu94::ui {

class TrackStrips : public QWidget {
public:
    /// (track_index, "volume"|"pan"|"mute"|"arm", value)
    /// volume: 0..150 (%), pan: -100..100, mute/arm: 0 or 1.
    using TrackHook = std::function<void(int, char const*, double)>;

    explicit TrackStrips(app::PlayEngine& engine, QWidget* parent = nullptr);

    void set_track_hook(TrackHook hook);

    /// UI-tick hook: re-sync the selected-row highlight.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct RowGeom {
        QRectF full;      // whole row
        QRectF chip;      // hue chip + name
        QRectF mute;      // M toggle
        QRectF arm;       // arm toggle
        QRectF vol;       // volume bar (4px + handle)
        QRectF pan;       // pan indicator lane
    };
    [[nodiscard]] RowGeom row_geom(int track) const noexcept;
    void set_volume(int track, double pct, bool report);
    void set_pan(int track, double pan, bool report);

    app::PlayEngine& engine_;
    TrackHook hook_;
    std::array<int, 4> volume_{100, 100, 100, 100};  // %
    std::array<int, 4> pan_{0, 0, 0, 0};             // -100..100
    std::array<bool, 4> mute_{false, false, false, false};
    std::array<bool, 4> arm_{false, false, false, false};
    int drag_track_{-1};       // -1 = none; 0..3
    bool drag_volume_{false};  // else pan
};

}  // namespace addu94::ui
