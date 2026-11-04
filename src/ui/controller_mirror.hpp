// addu94 — src/ui/controller_mirror.hpp
//
// ControllerMirror: custom-painted hardware mirror of the MPK Mini, styled
// to the shared theme (glass panels, inset wells, signature accent).
//
//   knobs    — premium 270° arcs: recessed base ring, gradient-filled knob
//              body, accent value arc with a hot tip, a soft glow that
//              fades over ~200 ms while the knob MOVES, tiny value +
//              tracked action label, and the resolved CC number in muted
//              micro text (so remaps are visible at a glance).
//   pads     — 2 banks of 8 with per-pad hue tints (drums warm, chords
//              violet, undo/redo teal) and a satisfying decaying hit glow.
//   mode     — segmented control (PLAY/EDIT/STEP), active segment filled
//              with the accent gradient; clickable → mapping().set_mode.
//   packs    — pad-pack selector chips (engine_.pad_packs()); the current
//              pack is highlighted, click → engine_.set_pad_pack(i)
//              (app-thread API, called from the UI thread only).
//   tracks   — track-role chips (engine_.track_name/track_role); the
//              selected track is accented, click → engine_.select_track(i)
//              — the user's answer to "keys stick on one output".
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
    /// bank B = 8..15 notes 44..51). Glow decays in paint.
    void pulse_pad(int pad_index);

    /// UI-tick hook: repaints while any pad glow, knob glow or value
    /// change is live.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void paint_knobs(QPainter& p, QRectF const& area);
    void paint_packs_tracks(QPainter& p, QRectF const& area);
    void paint_pads(QPainter& p, QRectF const& area);
    void paint_status(QPainter& p, QRectF const& area);
    /// One rounded chip (shared by pack + track rows); `on` gets the
    /// accent treatment, `hue` drives the identity dot on the left edge.
    void paint_chip(QPainter& p, QRectF const& r, QString const& text,
                    bool on, QColor const& hue, QFont const& font);

    app::PlayEngine& engine_;
    std::array<std::int64_t, 16> pad_hit_ms_{};  // steady-clock ms of last hit
    std::array<int, 8> knob_last_{};             // knob values at last paint
    std::array<std::int64_t, 8> knob_move_ms_{}; // when a knob last moved
    std::int64_t now_ms_{0};

    // Hit rects, cached during paint (pack count is engine-defined, so the
    // pack row keeps its own count; no per-frame allocation).
    QRectF mode_rects_[3]{};
    QRectF pack_rects_[16]{};
    int pack_count_{0};
    int pack_index_[16]{};
    QRectF track_rects_[4]{};
};

}  // namespace addu94::ui
