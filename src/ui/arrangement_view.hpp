// addu94 — src/ui/arrangement_view.hpp
//
// ArrangementView (spec §9): custom-painted timeline skeleton — bar ruler
// (from the transport meter), 4 track rows, 4-bar grid, playhead line at
// the transport position, and pattern blocks (rendered from the Block list;
// MainWindow feeds the current clip as a block until M5 wires the real
// arrangement). Clicking places a view-only cursor bar — transport editing
// is documented as NOT wired yet (lands with M5 scrub actions).
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's timer.
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>
#include <vector>

class QMouseEvent;
class QPainter;
class QPaintEvent;

namespace addu94::ui {

class ArrangementView : public QWidget {
public:
    struct Block {
        int track;        // 0..3
        int start_bar;    // first bar (0-based)
        int length_bars;  // > 0
    };

    explicit ArrangementView(app::PlayEngine& engine,
                             QWidget* parent = nullptr);

    /// Replace the rendered pattern blocks (small list; cheap at 30 Hz).
    void set_blocks(std::vector<Block> blocks);

    /// UI-tick hook: re-read playhead/selected track from the engine.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] int first_visible_bar() const noexcept;
    [[nodiscard]] double bar_width() const noexcept;

    app::PlayEngine& engine_;
    std::vector<Block> blocks_;
    double playhead_bars_{0.0};
    int cursor_bar_{0};  // view-only click cursor (no editing yet)
};

}  // namespace addu94::ui
