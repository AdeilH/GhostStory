// addu94 — src/ui/arrangement_view.hpp
//
// ArrangementView — the song canvas of the "one screen = the instrument"
// layout. Custom-painted: bar ruler with section-colored bands (intro /
// verse / chorus defaults at bars 0 / 8 / 16), 4 track lanes, big rounded
// pattern blocks (per-track hue gradient, soft shadow, name label), grid,
// glowing playhead with a soft trailing gradient, and loop-region shading
// from transport_control().loop_bars.
//
// Interactions (view state — engine arrangement wiring lands with M5):
//   click            → move the bar cursor
//   drag a block     → move it horizontally, snap to whole bars on release
//                      (stored in clip_start_bar_)
//   double-click     → empty lane: extend that lane's clip block +1 bar
//                      (or create a 1-bar block) — view state only
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's 30 Hz
// timer; set_clip_block() feeds the current clip as the selected track's
// block (deduped inside so drags are not clobbered).
#pragma once

#include "app/play_engine.hpp"

#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

class QMouseEvent;
class QPaintEvent;

namespace addu94::ui {

class ArrangementView : public QWidget {
public:
    struct Block {
        int track;        // 0..3
        int start_bar;    // first bar (0-based)
        int length_bars;  // > 0
        QString name;
    };

    explicit ArrangementView(app::PlayEngine& engine,
                             QWidget* parent = nullptr);

    /// Feed the current clip as a block on `track` with `length_bars`.
    /// Deduped: repeated identical calls (each UI tick) are no-ops, so an
    /// in-flight drag or user-extended length is not clobbered.
    void set_clip_block(int track, int length_bars);

    /// UI-tick hook: re-read playhead/loop from the engine and repaint.
    void refresh();

    /// View-state start bar of the current clip (drag target; engine
    /// integration comes later).
    [[nodiscard]] int clip_start_bar() const noexcept { return clip_start_bar_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int first_visible_bar() const noexcept;
    [[nodiscard]] double bar_width() const noexcept;
    [[nodiscard]] int bar_at(double x) const noexcept;
    [[nodiscard]] QRectF block_rect(Block const& b) const noexcept;
    void ensure_block(int track, int length_bars);

    app::PlayEngine& engine_;
    std::vector<Block> blocks_;
    double playhead_bars_{0.0};
    int cursor_bar_{0};
    int clip_start_bar_{0};   // view state: current clip start (drag target)
    int clip_len_bars_{0};    // last length fed by MainWindow (dedupe)

    // Drag state.
    bool dragging_{false};
    int drag_index_{-1};
    double drag_grab_offset_{0.0};  // bars between block start and grab point
    double drag_pos_{0.0};          // live (unsnapped) start during drag
};

}  // namespace addu94::ui
