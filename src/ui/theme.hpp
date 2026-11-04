// addu94 — src/ui/theme.hpp
//
// Shared visual language for the redesigned UI ("one screen = the
// instrument"): dark calm palette, panel geometry, track hues. Header-only
// so every custom-painted widget stays in sync without a translation unit.
//
//   base bg   #14161a   panels #1d2026   border #2a2f37
//   accent    #5aa2ff   record #ff5a5a
//   text      #e8ecf1   muted  #8b93a1
//   track hues T1..T4 = 210° / 160° / 30° / 300° (60% sat, desaturated)
#pragma once

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include <algorithm>

namespace addu94::ui::theme {

// Panels / geometry.
inline constexpr int kPanelRadius = 12;   // rounded corner radius
inline constexpr int kPanelPad = 8;       // inner padding
inline constexpr int kPanelGap = 12;      // space between panels

// Colors.
inline constexpr QColor kBaseBg{0x14, 0x16, 0x1a};
inline constexpr QColor kPanel{0x1d, 0x20, 0x26};
inline constexpr QColor kBorder{0x2a, 0x2f, 0x37};
inline constexpr QColor kAccent{0x5a, 0xa2, 0xff};
inline constexpr QColor kRecord{0xff, 0x5a, 0x5a};
inline constexpr QColor kText{0xe8, 0xec, 0xf1};
inline constexpr QColor kMuted{0x8b, 0x93, 0xa1};
inline constexpr QColor kGreen{0x37, 0xb2, 0x6c};
inline constexpr QColor kGrid{0x24, 0x28, 0x30};

// Track hues (degrees): T1..T4. Desaturated (60% sat), medium lightness —
// used for the track header chip and that track's pattern blocks.
inline constexpr int kTrackHue[4] = {210, 160, 30, 300};

[[nodiscard]] inline QColor track_color(int track) noexcept {
    return QColor::fromHsl(kTrackHue[track % 4], 153, 153);  // ~60% sat
}

[[nodiscard]] inline QColor with_alpha(QColor const& c, double a) noexcept {
    QColor out = c;
    out.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return out;
}

/// Cheap soft shadow: translucent black rounded rect offset +3px.
inline void draw_shadow(QPainter& p, QRectF const& r, double radius) noexcept {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawRoundedRect(r.translated(0.0, 3.0), radius, radius);
}

/// Standard panel: fill + 1px border, rounded corners.
inline void draw_panel(QPainter& p, QRectF const& r) noexcept {
    p.setPen(QPen(kBorder, 1.0));
    p.setBrush(kPanel);
    p.drawRoundedRect(r, kPanelRadius, kPanelRadius);
}

}  // namespace addu94::ui::theme
