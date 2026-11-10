// ghost_story — src/ui/track_strip.hpp
//
// TrackStrips: the left rail of the "one screen = the instrument" layout —
// a dynamic column of track cards in the shared glass language (start
// EMPTY; the "+ add instrument" bar at the bottom opens the dropdown of
// preset instruments + the shared Drum Kit, right-click a card to remove
// it). Each card carries a per-slot hue chip + the engine track name, a
// kind tag (SYNTH / DRUM KIT — where keys/pads land), mute (M) and arm
// (rec dot) toggles, a recessed volume lane (0..100%) with a draggable
// handle, and a mini pan lane (draggable, -100..100). The selected card is
// framed in the accent with a track-hue edge glow.
//
// The ENGINE is the mixer state: drags call engine_.set_track_volume/pan/
// mute/record_arm directly (per-index setters — the hardware TrackVolume
// knob acts on the selected track, strips address an explicit row), and
// refresh() reads the engine atomics back so hardware edits (and mute
// armed by other paths) stay in sync. Arm gates the loop recorder per
// track; with no track armed everything records (shipped behavior).
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

namespace ghost_story::ui {

class TrackStrips : public QWidget {
public:
    /// (track_index, "volume"|"pan"|"mute"|"arm", value)
    /// volume: 0..100 (%), pan: -100..100, mute/arm: 0 or 1.
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
    /// The "+ add instrument" bar: fixed height at the rail bottom (absent
    /// when the pool is full).
    [[nodiscard]] double add_bar_h() const noexcept {
        return engine_.track_count() < app::kTrackCount ? 26.0 : 0.0;
    }
    [[nodiscard]] QRectF add_bar_rect() const noexcept;
    void set_volume(int track, double pct, bool report);
    void set_pan(int track, double pan, bool report);

    app::PlayEngine& engine_;
    TrackHook hook_;
    // Display caches — refreshed from the engine each UI tick (the engine
    // atomics are the truth; these just avoid re-reading mid-drag values).
    std::array<int, app::kTrackCount> volume_{};
    std::array<int, app::kTrackCount> pan_{};
    std::array<bool, app::kTrackCount> mute_{};
    std::array<bool, app::kTrackCount> arm_{};
    int drag_track_{-1};       // -1 = none; active slot index
    bool drag_volume_{false};  // else pan
};

}  // namespace ghost_story::ui
