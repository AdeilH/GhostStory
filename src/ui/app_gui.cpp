// addu94 — src/ui/app_gui.cpp
#include "ui/app_gui.hpp"

#include "ui/main_window.hpp"
#include "ui/theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

#include <cstdlib>
#include <QString>

namespace addu94::ui {

namespace {

// Dark theme (spec §9) — keep in sync with ui/theme.hpp (custom-painted
// widgets hardcode the same values there).
void apply_dark_palette(QApplication& app) {
    QPalette pal;
    pal.setColor(QPalette::Window, theme::kBaseBg);
    pal.setColor(QPalette::WindowText, theme::kText);
    pal.setColor(QPalette::Base, theme::kPanel);
    pal.setColor(QPalette::AlternateBase, theme::kBaseBg);
    pal.setColor(QPalette::Text, theme::kText);
    pal.setColor(QPalette::PlaceholderText, theme::kMuted);
    pal.setColor(QPalette::Button, theme::kPanel);
    pal.setColor(QPalette::ButtonText, theme::kText);
    pal.setColor(QPalette::BrightText, theme::kRecord);
    pal.setColor(QPalette::Highlight, theme::kAccent);
    pal.setColor(QPalette::HighlightedText, theme::kBaseBg);
    pal.setColor(QPalette::ToolTipBase, theme::kPanel);
    pal.setColor(QPalette::ToolTipText, theme::kText);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, theme::kMuted);
    pal.setColor(QPalette::Disabled, QPalette::Text, theme::kMuted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, theme::kMuted);
    app.setPalette(pal);
}

}  // namespace

int run_gui(app::PlayEngine& engine, int auto_stop_seconds) {
    // QApplication wants (int&, char**); the binary's real argv is parsed
    // in main() already — pass a minimal one. No display dependency is
    // required for construction (QT_QPA_PLATFORM=offscreen works).
    static char app_name[] = "addu94";
    char* argv[] = {app_name, nullptr};
    int argc = 1;
    QApplication app(argc, argv);

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    apply_dark_palette(app);
    QApplication::setApplicationName("addu94");
    QApplication::setApplicationVersion("0.1.0-dev");

    MainWindow window(engine);
    window.show();

    // UI test aid (permanent): A94_SHOT=<path> grabs the window 2 s after
    // startup and quits. A94_SHOT_PLAY=1 additionally starts the transport
    // shortly after startup, so screenshots can capture the live state
    // (playhead, loop shading, level meter). Used for the offscreen
    // screenshot loop:
    //   A94_SHOT=/tmp/ui.png QT_QPA_PLATFORM=offscreen ./build/main/addu94 play
    if (char const* shot = std::getenv("A94_SHOT"); shot != nullptr) {
        QString const path = QString::fromUtf8(shot);
        if (std::getenv("A94_SHOT_PLAY") != nullptr) {
            QTimer::singleShot(300, &app, [&engine] {
                auto& ctrl = engine.audio().transport_control();
                ctrl.state_req.store(music::TransportControl::kReqPlay,
                                     std::memory_order_release);
                ctrl.metronome_on.store(true, std::memory_order_relaxed);
                ctrl.loop_bars.store(4, std::memory_order_relaxed);
            });
        }
        QTimer::singleShot(2000, &app, [&app, &window, path] {
            window.grab().save(path);
            app.quit();
        });
    }

    if (auto_stop_seconds > 0) {
        QTimer::singleShot(auto_stop_seconds * 1000, &app, [&app] {
            app.quit();
        });
    }

    return QApplication::exec();
}

}  // namespace addu94::ui
