// ghost_story — src/ui/arrangement_view.hpp
//
// ArrangementView — the song canvas of the "one screen = the instrument"
// layout. Custom-painted: bar ruler with section-colored bands (intro /
// verse/ chorus defaults at bars 0 / 8 / 16), one lane per active track,
// big rounded pattern blocks (per-track hue gradient, soft shadow, name
// label), grid, glowing playhead with a soft trailing gradient, and
// loop-region shading from transport_control().loop_bars.
//
// REAL CLIPS (loop machine): each lane shows that track's actual recording
// fed from the engine every UI tick — the blocks ARE the takes, not copies.
// Every recorded take draws its OWN block ([rec 1] [rec 2] both visible,
// user report: "it removes rec 1 — we want both"), while a track with no
// recorded takes draws its reserved window as a faint dashed outline.
//   click anywhere        → move the playhead (play/record start there)
//   drag a block's edge   → resize THAT take, engine-committed on release
//   drag a block body     → move the track's whole take-group along the
//                           timeline (snaps to bars)
//   double-click a block  → extend that take +1 bar
//   double-click a lane   → reserve a 1-bar window at the clicked bar
//   right-click a block   → clear THAT take's recording
// Blocks for removed tracks are pruned every tick — the canvas always
// mirrors engine truth (the stale-ghost bug: removing instruments left
// dead blocks painted over "no instruments yet").
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's 30 Hz
// timer.
#pragma once

#include "app/play_engine.hpp"

#include <QColor>
#include <QString>
#include <QWidget>
#include <array>
#include <vector>

class QMouseEvent;
class QPaintEvent;

namespace ghost_story::ui {

class ArrangementView : public QWidget {
public:
    struct Block {
        int track;        // active slot index
        int take;         // the take this box shows (-1 = reserved window)
        int start_bar;    // placement (absolute, in bars)
        int length_bars;  // > 0
        bool has_notes;   // false = reserved/provisional, nothing recorded
    };

    explicit ArrangementView(app::PlayEngine& engine,
                             QWidget* parent = nullptr);

    /// Feed track `track`'s real takes (the engine's clip boxes) plus its
    /// reserved window (drawn only while nothing was recorded on the
    /// track). Deduped per track: repeated identical calls (each UI tick)
    /// are no-ops so repaints stay cheap.
    void set_track_clips(
        int track,
        std::vector<app::PlayEngine::ClipBoxView> const& boxes,
        int reserved_start_bar, int reserved_bars, bool reserved_has_notes);

    /// UI-tick hook: re-read playhead/loop from the engine, prune blocks
    /// of removed tracks, repaint.
    void refresh();

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
    /// Index into blocks_ of the block whose resize edge (±6px) is under
    /// (x, y), or -1.
    [[nodiscard]] int edge_block_index_at(QPointF const& pos) const noexcept;
    /// Index into blocks_ of the block under (x, y), or -1.
    [[nodiscard]] int block_index_at(QPointF const& pos) const noexcept;
    /// Blocks are identified by (track, take) across feeds — a rebuild
    /// between press and release must not retarget a drag.
    [[nodiscard]] int block_index_of(int track, int take) const noexcept;

    app::PlayEngine& engine_;
    std::vector<Block> blocks_;  // one per take box (+ reserved windows)
    double playhead_bars_{0.0};

    // Feed dedupe, per track: the last boxes + reserved window seen.
    std::array<std::vector<app::PlayEngine::ClipBoxView>, app::kTrackCount>
        fed_boxes_{};
    std::array<int, app::kTrackCount> fed_reserved_start_{};
    std::array<int, app::kTrackCount> fed_reserved_bars_{};
    std::array<bool, app::kTrackCount> fed_reserved_notes_{};

    // Edge-resize drag state (engine-committed on release).
    bool resizing_{false};
    int resize_track_{-1};
    int resize_take_{-1};
    int resize_bars_{0};  // live (uncommitted) length during the drag

    // Move drag: the track's whole take-group relocates ("record a portion
    // and drop it in the play bar") — body-drag moves, snap-to-bar on
    // release, committed through engine_.set_clip_start with the window
    // start derived from the dragged box's delta.
    bool moving_{false};
    int move_track_{-1};
    int move_take_{-1};
    int move_window_start_{0};   // the group's window start at press
    double move_grab_offset_{0.0};
    int move_box_start_{0};      // the grabbed box's start at press
    int move_start_bar_{0};      // live (uncommitted) placement during drag
};

}  // namespace ghost_story::ui
