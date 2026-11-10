// ghost_story — src/ui/app_gui.cpp
#include "ui/app_gui.hpp"

#include "ui/main_window.hpp"
#include "ui/theme.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QSettings>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <QString>

namespace ghost_story::ui {

namespace {

/// Prefer an installed humanist sans over the X11 default; silent
/// fallback when none of the candidates exists (fontconfig picks up the
/// slack via the style hints in theme.hpp). One quiet decision at
/// startup — typography is half of not looking machine-generated.
void set_app_font(QApplication& app) {
    for (char const* family :
         {"Inter", "Noto Sans", "Open Sans", "DejaVu Sans"}) {
        if (QFontDatabase::hasFamily(QString::fromUtf8(family))) {
            QFont f{QString::fromUtf8(family)};
            app.setFont(f);
            return;
        }
    }
}

}  // namespace

// Theme-aware system palette — derived from ui/theme.hpp so the few real
// Qt widgets (combo popups, dialogs, tooltips) match the hand-painted
// panels of whatever theme is active. Re-run on every theme switch.
void apply_app_palette(QApplication& app) {
    QPalette pal;
    pal.setColor(QPalette::Window, theme::pal().base_top);
    pal.setColor(QPalette::WindowText, theme::pal().text);
    pal.setColor(QPalette::Base, theme::pal().panel_top);
    pal.setColor(QPalette::AlternateBase, theme::pal().base_bottom);
    pal.setColor(QPalette::Text, theme::pal().text);
    pal.setColor(QPalette::PlaceholderText, theme::pal().muted);
    pal.setColor(QPalette::Button, theme::pal().panel_top);
    pal.setColor(QPalette::ButtonText, theme::pal().text);
    pal.setColor(QPalette::BrightText, theme::pal().record);
    pal.setColor(QPalette::Highlight, theme::pal().accent);
    pal.setColor(QPalette::HighlightedText,
                 theme::is_light() ? theme::pal().panel_top
                                   : theme::pal().base_top);
    pal.setColor(QPalette::ToolTipBase, theme::pal().panel_top);
    pal.setColor(QPalette::ToolTipText, theme::pal().text);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, theme::pal().muted);
    pal.setColor(QPalette::Disabled, QPalette::Text, theme::pal().muted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, theme::pal().muted);
    app.setPalette(pal);
}

int run_gui(app::PlayEngine& engine, int auto_stop_seconds) {
    // QApplication wants (int&, char**); the binary's real argv is parsed
    // in main() already — pass a minimal one. No display dependency is
    // required for construction (QT_QPA_PLATFORM=offscreen works).
    static char app_name[] = "ghost_story";
    char* argv[] = {app_name, nullptr};
    int argc = 1;
    QApplication app(argc, argv);

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setApplicationName("Ghost Story");
    // QSettings (window size / theme persistence) keys off these two.
    QApplication::setOrganizationName("ghost_story");
    QApplication::setApplicationVersion("0.1.0-dev");

    // Load the saved theme BEFORE the palette and MainWindow — the first
    // frame must already be in the user's theme, not a two-step flip.
    QSettings settings;
    theme::set_theme(settings.value("ui/theme", 0).toInt());
    // UI test aid: GS_SHOT_THEME=<name or index> pins the theme so
    // screenshot runs can verify every palette (see GS_SHOT below).
    if (char const* theme_env = std::getenv("GS_SHOT_THEME");
        theme_env != nullptr) {
        for (int i = 0; i < theme::kThemeCount; ++i) {
            if (std::strcmp(theme::theme_name(i), theme_env) == 0) {
                theme::set_theme(i);
                break;
            }
        }
        char* end = nullptr;
        long const idx = std::strtol(theme_env, &end, 10);
        if (end != theme_env && *end == '\0') theme::set_theme(int(idx));
    }
    apply_app_palette(app);
    set_app_font(app);

    MainWindow window(engine);
    window.show();

    // UI test aid (permanent): GS_SHOT=<path> grabs the window 2 s after
    // startup and quits. GS_SHOT_PLAY=1 additionally starts the transport
    // shortly after startup, so screenshots can capture the live state
    // (playhead, loop shading, level meter). GS_SHOT_SIZE=WxH resizes the
    // window first, so layouts can be verified at any size (e.g. the
    // 960x560 floor). Used for the offscreen screenshot loop:
    //   GS_SHOT=/tmp/ui.png QT_QPA_PLATFORM=offscreen ./build/main/ghost_story play
    if (char const* size_env = std::getenv("GS_SHOT_SIZE");
        size_env != nullptr) {
        int w = 0, h = 0;
        if (std::sscanf(size_env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            window.resize(w, h);
        }
    }
    window.show();
    if (char const* shot = std::getenv("GS_SHOT"); shot != nullptr) {
        QString const path = QString::fromUtf8(shot);
        if (std::getenv("GS_SHOT_PLAY") != nullptr) {
            QTimer::singleShot(300, &app, [&engine] {
                // Tracks are dynamic and start EMPTY — the screenshot aid
                // seeds a synth + drums so the "playing" captures show the
                // populated instrument (same aid spirit as kReqPlay).
                if (engine.track_count() == 0) {
                    std::string err;
                    (void)engine.add_track(0, err);   // Default Saw
                    (void)engine.add_track(-1, err);  // Drums
                }
                auto& ctrl = engine.audio().transport_control();
                ctrl.state_req.store(music::TransportControl::kReqPlay,
                                     std::memory_order_release);
                ctrl.loop_bars.store(4, std::memory_order_relaxed);
            });
        }
        // GS_SHOT_REC=1 (with PLAY): punch recording in 0.5 s after the
        // transport rolls, so captures show the take-placement flow — the
        // block grown from the playhead bar with its live REC badge.
        if (std::getenv("GS_SHOT_REC") != nullptr) {
            QTimer::singleShot(800, &app, [&engine] {
                engine.command({mapping::Action::TransportRecord, 0, 0});
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

}  // namespace ghost_story::ui
