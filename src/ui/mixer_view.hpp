// addu94 — src/ui/mixer_view.hpp
//
// MixerView (spec §9): 4 track strips — volume (0..150, default 100), pan
// (-100..100), mute/solo toggles. Values live in the widget for now;
// changes are reported through the track_changed hook, which MainWindow
// wires to the log until M5/M6 connect real mixer state.
//
// Plain widget (no Q_OBJECT): signals handled with lambdas.
#pragma once

#include <QWidget>
#include <functional>
#include <vector>

class QLabel;
class QPushButton;
class QSlider;

namespace addu94::ui {

class MixerView : public QWidget {
public:
    /// (track_index, "volume"|"pan"|"mute"|"solo", value)
    using TrackHook = std::function<void(int, char const*, double)>;

    explicit MixerView(QWidget* parent = nullptr);

    void set_track_hook(TrackHook hook);

private:
    TrackHook hook_;
    std::vector<QSlider*> volume_;
    std::vector<QSlider*> pan_;
    std::vector<QPushButton*> mute_;
    std::vector<QPushButton*> solo_;
};

}  // namespace addu94::ui
