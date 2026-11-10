// ghost_story — src/ui/fx_panel.hpp
//
// FxPanel: the per-track FX + preset control surface, styled to the shared
// theme (glass panel, inset wells, signature accent). Rows:
//
//   FX   [EQ][Comp][Crush][Drive][Chorus][Flanger][Delay][Trem][Verb]
//        ↑ click = select slot (hardware knob 1 in EDIT follows the same
//          engine_.set_fx_slot state); a lit chip = its block is ON; the
//          selected slot is accented.
//        A [====|----] low dB   B [==|------] high dB
//        ↑ click/drag = the selected slot's param A/B. The lanes send the
//          SAME mapping::Command (FxParamA/FxParamB, 0..127) the hardware
//          knobs produce, so the UI and the MPK knobs are one code path.
//   presets: one dropdown — "(custom)" + the ten curated
//            {synth patch + FX chain} loads, applied to the SELECTED track
//            (drums track: packs are the presets, so the combo disables
//            itself with a hint instead of erroring on every pick).
//
// Plain widget (no Q_OBJECT): repaints are driven by MainWindow's 30 Hz
// timer via refresh(). The preset combo is the one real child widget
// (same pattern as HealthStrip's combos — lambdas, no moc needed).
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>
#include <array>
#include <QRectF>
#include <QString>

class QComboBox;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;

namespace ghost_story::ui {

class FxPanel : public QWidget {
public:
    explicit FxPanel(app::PlayEngine& engine, QWidget* parent = nullptr);

    /// UI-tick hook: repaint (state lives in the engine; this widget keeps
    /// no truth beyond drag bookkeeping), keep the preset combo synced
    /// with engine truth, and re-style it after a theme switch.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    /// Row rects for the current size — the single source both paintEvent
    /// and resizeEvent (which must place the preset combo inside the
    /// preset row) read, so the widget and its painted row never drift.
    /// Chip rects are computed here too (font-metric aware): the band
    /// height, the painted chips and the hit rects can't disagree.
    struct Geom {
        std::array<QRectF, app::PlayEngine::kFxSlotCount> chip_rects;
        int chip_rows;   // rows the slot chips wrapped to (1 or 2)
        QRectF chips;    // row 1 band containing those chip rows
        QRectF lanes;    // row 2: the A/B param lanes
        QRectF presets;  // row 3: the preset combo (tag column excluded)
        QRectF desc;     // bottom strip: fx description / last error
    };
    [[nodiscard]] Geom compute_layout() const;
    /// Normalized 0..1 display value of the selected slot's param `ab` for
    /// the SELECTED track (mirrors the fx_knob ranges in PlayEngine).
    [[nodiscard]] double param_value(int ab) const;
    /// Fill the preset combo for the selected track (synth tracks get
    /// "(custom)" + the presets; drums get a disabled hint row).
    void repopulate_presets();

    app::PlayEngine& engine_;

    // Hit rects, cached during paint (fixed counts, no per-frame alloc).
    QRectF slot_rects_[app::PlayEngine::kFxSlotCount]{};
    QRectF param_rects_[2]{};

    // Lane drag state: -1 = not dragging; else which lane (0=A, 1=B).
    int dragging_{-1};

    // Cached for the drag -> param conversion (lane x at press).
    double drag_origin_x_{0.0};
    double drag_origin_value_{0.0};

    // Preset selector. Combo row 0 is the "(custom)" placeholder — engine
    // row r lives at combo row r+1; -1 (manual edits) shows row 0.
    QComboBox* preset_combo_{nullptr};
    int preset_combo_track_{-1};  // track the items were populated for
    int theme_generation_{-1};    // restyle the combo when generation() moves

    QString preset_error_;  // last apply_preset error (bottom strip, red)
};

}  // namespace ghost_story::ui
