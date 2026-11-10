// ghost_story — src/ui/mic_panel.hpp
//
// MicPanel: the live-input channel strip, sitting right of the arrangement
// canvas. Everything a laptop performer needs to sing/play INTO the beat:
//
//   status   — green dot while the capture path is actually delivering
//              frames (engine mic_active(); dims when no device opened or
//              input went silent).
//   monitor  — on/off chip: mix the mic into the master (pre-fader).
//              Default ON; the tooltip warns about speaker feedback.
//   gain     — 0..200% lane, applied before both the monitor mix and the
//              grab history.
//   level    — post-gain peak bar from engine mic_level() (30 fps decay).
//   grab     — the sampler trick: take the last 1/2/4 s of captured mic
//              audio and load it as a drum pad zone (mono, peak
//              normalized). Duration chips + 8 bank-A pad targets + the
//              GRAB button; the result/error line reports what happened.
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's 30 Hz
// timer; grabs report success through grab_hook so MainWindow can pulse
// the pad in the ControllerMirror.
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>
#include <QRectF>
#include <functional>

class QMouseEvent;
class QPaintEvent;

namespace ghost_story::ui {

class MicPanel : public QWidget {
public:
    /// (pad_index 0..7) fired after a successful grab — MainWindow pulses
    /// the pad in the mirror.
    using GrabHook = std::function<void(int)>;

    explicit MicPanel(app::PlayEngine& engine, QWidget* parent = nullptr);

    void set_grab_hook(GrabHook hook) { grab_hook_ = std::move(hook); }

    /// UI-tick hook: repaint (level meter decay is in paint).
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    app::PlayEngine& engine_;
    GrabHook grab_hook_;

    // Hit rects, cached during paint.
    QRectF monitor_rect_{};
    QRectF gain_rect_{};
    QRectF dur_rects_[3]{};
    QRectF pad_rects_[8]{};
    QRectF grab_rect_{};
    bool dragging_gain_{false};

    // Grab state (UI only).
    int grab_seconds_{2};   // 1 / 2 / 4
    int grab_pad_{0};       // bank A pad 0..7 (note 36+pad)
    QString grab_status_{}; // last result line (or error)
    bool grab_error_{false};
    float level_{0.0f};     // decayed mic level (0..1)
};

}  // namespace ghost_story::ui
