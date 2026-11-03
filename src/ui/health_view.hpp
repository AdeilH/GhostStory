// addu94 — src/ui/health_view.hpp
//
// HealthView (spec §9): audio backend identity, live callback stats
// (callbacks/xruns/last-max µs/sample rate/buffer), GUI uptime, and a
// "Bench" button that re-runs this binary's `bench --seconds 10` subcommand
// in a child process (QProcess) and shows the output tail.
//
// Plain widget (no Q_OBJECT): signals handled with lambdas.
#pragma once

#include "app/play_engine.hpp"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;

namespace addu94::ui {

class HealthView : public QWidget {
public:
    explicit HealthView(app::PlayEngine& engine, QWidget* parent = nullptr);

    /// UI-tick hook: stats readout.
    void refresh();

private:
    void start_bench();

    app::PlayEngine& engine_;
    QElapsedTimer uptime_;
    QLabel* backend_;
    QLabel* stats_;
    QLabel* xruns_;
    QPlainTextEdit* bench_output_;
    QPushButton* bench_btn_;
    QProcess* bench_proc_{nullptr};
};

}  // namespace addu94::ui
