// ghost_story — src/ui/controller_mirror.hpp
//
// ControllerMirror: custom-painted hardware mirror of the MPK Mini, styled
// to the shared theme (glass panels, inset wells, signature accent).
//
//   knobs    — premium 270° arcs: recessed base ring, gradient-filled knob
//              body, accent value arc with a hot tip, a soft glow that
//              fades over ~200 ms while the knob MOVES, tiny value +
//              tracked action label, and the resolved CC number in muted
//              micro text (so remaps are visible at a glance). INTERACTIVE:
//              drag vertically, wheel nudge, double-click reset — every
//              move emits the same mapping::Command the hardware knob
//              would (knob_action), so the on-screen knobs ARE the knobs.
//   pads     — 2 banks of 8 with per-pad hue tints (each kit voice keeps
//              its hue across both banks — B is A one octave up) and a
//              satisfying decaying hit glow. CLICKABLE per mode (PLAY
//              drums, EDIT select/undo/chord press-release, KIT packs).
//   mode     — segmented control (PLAY/EDIT/KIT), active segment filled
//              with the accent gradient; clickable → engine set_mode
//              (keeps the MIDI-thread pad gate in sync).
//   packs    — pad-pack dropdown (engine_.pad_packs()); the current pack
//              is shown, pick → engine_.set_pad_pack(i) (app-thread API,
//              called from the UI thread only). Replaced the old 16-chip
//              grid, which wrapped to two rows of 6pt text — KIT-mode pads
//              remain the fast performance path for pack switching.
//   tracks   — track-role chips (engine_.track_name/track_role); the
//              selected track is accented, click → engine_.select_track(i)
//              — the user's answer to "keys stick on one output".
//
// Plain widget (no Q_OBJECT): repaints are driven by MainWindow's 30 Hz
// timer via refresh(); hits arrive via pulse_pad() from the same drain.
// The pack dropdown is the one real child widget (same pattern as
// HealthStrip's combos — lambdas, no moc needed).
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>
#include <array>
#include <cstdint>
#include <vector>

class QComboBox;
class QMouseEvent;
class QPainter;
class QPaintEvent;
class QRectF;
class QResizeEvent;
class QWheelEvent;

namespace ghost_story::ui {

class ControllerMirror : public QWidget {
public:
    explicit ControllerMirror(app::PlayEngine& engine,
                              QWidget* parent = nullptr);

    /// Pad hit feedback: absolute index 0..15 (bank A = 0..7 notes 36..43,
    /// bank B = 8..15 notes 44..51). Glow decays in paint.
    void pulse_pad(int pad_index);

    /// UI-tick hook: repaints while any pad glow, knob glow or value
    /// change is live, keeps the pack dropdown synced with the engine, and
    /// re-styles it after a theme switch.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    /// Section rects for the current size — the single source both
    /// paintEvent and resizeEvent (which must place the pack combo inside
    /// the packs row) read, so the widget and its child never drift apart.
    struct Geom {
        QRectF knobs;
        QRectF packs;  // the row the pack dropdown lives in
        QRectF pads;
        QRectF status;
    };
    [[nodiscard]] Geom compute_layout() const;
    void paint_knobs(QPainter& p, QRectF const& area);
    void paint_pack_tag(QPainter& p, QRectF const& area);
    void paint_pads(QPainter& p, QRectF const& area);
    void paint_status(QPainter& p, QRectF const& area);
    /// UI-tick: pull engine truth into the knob display for the knobs
    /// with a clean inverse map (PLAY vol/pan/send, master) — see below.
    void sync_knob_display();
    /// One rounded chip (shared by track + keys rows); `on` gets the
    /// accent treatment, `hue` drives the identity dot on the left edge
    /// (suppressed on chips too narrow to carry text plus a dot).
    void paint_chip(QPainter& p, QRectF const& r, QString const& text,
                    bool on, QColor const& hue, QFont const& font,
                    bool dot = true);
    /// Pad cell geometry (shared by paint + hit-testing): absolute index
    /// 0..15 → its rect inside the pads well; an invalid rect when the
    /// layout has not been painted yet.
    [[nodiscard]] QRectF pad_cell(int idx) const noexcept;
    /// Absolute pad index (0..15) under `pos`, or -1.
    [[nodiscard]] int pad_at(QPointF const& pos) const noexcept;
    /// Fire a pad press per the current mode (the same commands map_note
    /// emits for hardware). `pressed` selects press/release semantics.
    void press_pad(int idx, bool pressed);

    app::PlayEngine& engine_;
    std::array<std::int64_t, 16> pad_hit_ms_{};  // steady-clock ms of last hit
    std::array<int, 8> knob_last_{};             // knob values at last paint
    std::array<std::int64_t, 8> knob_move_ms_{}; // when a knob last moved
    std::int64_t now_ms_{0};

    // Hit rects, cached during paint (no per-frame allocation).
    QRectF mode_rects_[3]{};
    QRectF track_rects_[4]{};
    QRectF keys_rects_[2]{};  // 0 = "once", 1 = "repeat"
    QRectF knob_rects_[8]{};  // arc hit targets (grown slightly)
    QRectF pads_rect_{};      // the pads well, for pad_cell()
    int drag_knob_{-1};       // -1 = none; 0..7 while a knob drag is live
    double drag_knob_y0_{0.0};
    int drag_knob_v0_{0};
    int held_chord_pad_{-1};  // EDIT chord slot pressed via mouse, 0..5

    // Pack selector. pack_combo_index_[row] is the engine pack index behind
    // combo row `row` (kept parallel to pad_packs() instead of assuming
    // index == position).
    QComboBox* pack_combo_{nullptr};
    std::vector<int> pack_combo_index_;
    int theme_generation_{-1};  // restyle the combo when generation() moves
};

}  // namespace ghost_story::ui
