// addu94 — src/ui/app_gui.cpp
#include "ui/app_gui.hpp"

#include "ui/main_window.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

namespace addu94::ui {

namespace {

// Dark theme (spec §9). Custom-painted widgets (ControllerMirror,
// ArrangementView) hardcode the same colors.
constexpr QColor kWindow{0x1a, 0x1d, 0x21};   // #1a1d21
constexpr QColor kPanel{0x23, 0x27, 0x2c};    // #23272c
constexpr QColor kBlue{0x4f, 0x9c, 0xf9};     // #4f9cf9
constexpr QColor kText{0xcf, 0xd6, 0xdd};     // #cfd6dd
constexpr QColor kDim{0x6b, 0x74, 0x7e};

void apply_dark_palette(QApplication& app) {
    QPalette pal;
    pal.setColor(QPalette::Window, kWindow);
    pal.setColor(QPalette::WindowText, kText);
    pal.setColor(QPalette::Base, kPanel);
    pal.setColor(QPalette::AlternateBase, kWindow);
    pal.setColor(QPalette::Text, kText);
    pal.setColor(QPalette::PlaceholderText, kDim);
    pal.setColor(QPalette::Button, kPanel);
    pal.setColor(QPalette::ButtonText, kText);
    pal.setColor(QPalette::BrightText, QColor(0xd9, 0x53, 0x4f));
    pal.setColor(QPalette::Highlight, kBlue);
    pal.setColor(QPalette::HighlightedText, kWindow);
    pal.setColor(QPalette::ToolTipBase, kPanel);
    pal.setColor(QPalette::ToolTipText, kText);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, kDim);
    pal.setColor(QPalette::Disabled, QPalette::Text, kDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, kDim);
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

    if (auto_stop_seconds > 0) {
        QTimer::singleShot(auto_stop_seconds * 1000, &app, [&app] {
            app.quit();
        });
    }

    return QApplication::exec();
}

}  // namespace addu94::ui
