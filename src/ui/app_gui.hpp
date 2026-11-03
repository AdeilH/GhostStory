// addu94 — src/ui/app_gui.hpp
//
// GUI entry point (spec §9): builds the dark QApplication (Fusion style,
// system-level palette) and runs the MainWindow until quit.
//
// auto_stop_seconds > 0 quits the event loop after that many seconds —
// used by `addu94 play --seconds N` so CI/headless smoke runs exit 0.
#pragma once

#include "app/play_engine.hpp"

namespace addu94::ui {

/// Runs the native GUI on the calling thread. Returns QApplication::exec().
int run_gui(app::PlayEngine& engine, int auto_stop_seconds = 0);

}  // namespace addu94::ui
