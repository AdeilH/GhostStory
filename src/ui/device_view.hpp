// addu94 — src/ui/device_view.hpp
//
// DeviceView (spec §9): MPK connection status, probe summary (from the
// device manifest under ~/.local/share/addu94/devices/), the live knob
// mirror table, and the velocity-curve selector (stored in-widget until
// M6 wires the synth-side curve).
//
// Plain widget (no Q_OBJECT): refresh() is called from MainWindow's timer.
#pragma once

#include "app/play_engine.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTableWidget;

namespace addu94::ui {

class DeviceView : public QWidget {
public:
    explicit DeviceView(app::PlayEngine& engine, QWidget* parent = nullptr);

    /// UI-tick hook: connection LED, knob values.
    void refresh();

private:
    void load_manifest_summary();

    app::PlayEngine& engine_;
    QLabel* status_;
    QPlainTextEdit* summary_;
    QTableWidget* knobs_;
    QComboBox* velocity_curve_;
};

}  // namespace addu94::ui
