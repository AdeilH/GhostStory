// ghost_story — src/ui/app_gui.hpp
//
// GUI entry point (spec §9): builds the QApplication (Fusion style,
// theme-aware system palette) and runs the MainWindow until quit.
//
// auto_stop_seconds > 0 quits the event loop after that many seconds —
// used by `ghost_story play --seconds N` so CI/headless smoke runs exit 0.
#pragma once

#include "app/play_engine.hpp"

class QApplication;

namespace ghost_story::ui {

/// Runs the native GUI on the calling thread. Returns QApplication::exec().
int run_gui(app::PlayEngine& engine, int auto_stop_seconds = 0);

/// Mirrors theme::pal() onto the QApplication-level QPalette so the few
/// real Qt widgets (combo popups, dialogs, tooltips) match the
/// hand-painted panels. Cheap; called on startup and again whenever the
/// active theme changes (MainWindow's tick watches theme::generation()).
void apply_app_palette(QApplication& app);

}  // namespace ghost_story::ui
