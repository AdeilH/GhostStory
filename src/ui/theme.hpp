// addu94 — src/ui/theme.hpp
//
// Shared visual language for the redesigned UI ("one screen = the
// instrument"). Header-only so every custom-painted widget stays in sync
// without a translation unit.
//
// The look: a deep near-black stage with a faint vertical falloff (the
// window reads as a lit surface, not flat paint), and slightly luminous
// "glass" panels — soft drop shadow, hairline border, and a top-edge
// highlight that fades down the sides like light catching a bezel. One
// strong signature accent (electric blue) carries selection and motion;
// each track keeps its own desaturated hue for chips, blocks and meters.
//
// Typography has three tiers, used consistently everywhere:
//   labels  — tiny, uppercase, letter-spaced, muted   (what is this?)
//   values  — demi-bold, one step larger, full text   (what does it say?)
//   hints   — tiny regular, muted                     (how do I use it?)
//
//   base bg    #0d0f13 → #15171c (vertical gradient)
//   panel      #21252d → #191c22 (vertical gradient, glass)
//   hairline   #2c313b   top highlight rgba(255,255,255, ~0.05)
//   accent     #4f9dff (electric blue)   record #ff5a5a
//   text       #e9edf3   muted #8a93a3   well (inset) #14161b
//   track hues T1..T4 = 212° / 165° / 25° / 292° (~62% sat)
//   role hues  synths → blue-violet family, drums → amber, bass → magenta
//
// Performance contract: all helpers are inline, allocation-free, and safe
// to call at the 30 Hz repaint budget (gradients are stack objects).
#pragma once

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include <algorithm>

namespace addu94::ui::theme {

// Panels / geometry.
inline constexpr int kPanelRadius = 12;   // rounded corner radius
inline constexpr int kPanelPad = 8;       // inner padding
inline constexpr int kPanelGap = 12;      // space between panels

// --- colors ------------------------------------------------------------
inline constexpr QColor kBaseTop{0x0d, 0x0f, 0x13};     // window gradient top
inline constexpr QColor kBaseBottom{0x15, 0x17, 0x1c};  // window gradient bottom
inline constexpr QColor kPanelTop{0x21, 0x25, 0x2d};    // panel gradient top
inline constexpr QColor kPanelBottom{0x19, 0x1c, 0x22}; // panel gradient bottom
inline constexpr QColor kBorder{0x2c, 0x31, 0x3b};      // hairline border
inline constexpr QColor kWell{0x14, 0x16, 0x1b};        // inset track/arc bg
inline constexpr QColor kAccent{0x4f, 0x9d, 0xff};      // signature electric blue
inline constexpr QColor kAccentHot{0x9c, 0xcb, 0xff};   // bright tip of arcs
inline constexpr QColor kRecord{0xff, 0x5a, 0x5a};
inline constexpr QColor kText{0xe9, 0xed, 0xf3};
inline constexpr QColor kMuted{0x8a, 0x93, 0xa3};
inline constexpr QColor kGreen{0x3e, 0xcf, 0x8e};
inline constexpr QColor kGrid{0x22, 0x26, 0x2e};        // faint grid lines

// Track hues (degrees): T1..T4. Desaturated (~62% sat), medium lightness —
// used for the track header chip, that track's pattern blocks and meters.
inline constexpr int kTrackHue[4] = {212, 165, 25, 292};

[[nodiscard]] inline QColor track_color(int track) noexcept {
    return QColor::fromHsl(kTrackHue[track % 4], 158, 152);
}

// Role labels/hues, indexed by app::TrackRole's underlying value
// (0=SynthA 1=SynthB 2=DrumKit 3=Bass). Kept as plain ints so this header
// does not have to pull in play_engine.hpp. WHY the UI should speak roles:
// the hardware mapping routes keys/pads per role, so labeling rows by role
// (not just name) tells the player where input will land.
[[nodiscard]] inline char const* role_label(int role) noexcept {
    switch (role % 4) {
    case 0: return "SYNTH A";
    case 1: return "SYNTH B";
    case 2: return "DRUM KIT";
    default: return "BASS";
    }
}

[[nodiscard]] inline QColor role_color(int role) noexcept {
    switch (role % 4) {
    case 0: return QColor::fromHsl(212, 158, 152);
    case 1: return QColor::fromHsl(262, 130, 160);
    case 2: return QColor::fromHsl(25, 150, 150);
    default: return QColor::fromHsl(292, 140, 156);
    }
}

[[nodiscard]] inline QColor with_alpha(QColor const& c, double a) noexcept {
    QColor out = c;
    out.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return out;
}

// --- typography tiers ---------------------------------------------------
/// Tiny uppercase tracked-out label (what is this?).
[[nodiscard]] inline QFont label_font(QFont const& base) noexcept {
    QFont f = base;
    f.setPointSize(std::max(base.pointSize() - 3, 6));
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    return f;
}

/// Demi-bold value (what does it say?).
[[nodiscard]] inline QFont value_font(QFont const& base, int pt = 13) noexcept {
    QFont f = base;
    f.setPointSize(std::max(pt, 8));
    f.setWeight(QFont::DemiBold);
    return f;
}

/// Monospaced readout (transport position etc.).
[[nodiscard]] inline QFont mono_font(QFont const& base, int pt = 11) noexcept {
    QFont f = base;
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(std::max(pt, 9));
    f.setWeight(QFont::Medium);
    return f;
}

// --- painting primitives -------------------------------------------------
/// Cheap soft shadow: translucent black rounded rect offset +3px.
inline void draw_shadow(QPainter& p, QRectF const& r, double radius) noexcept {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 85));
    p.drawRoundedRect(r.translated(0.0, 3.0), radius, radius);
}

/// Glass panel: soft shadow + vertical luminous fill + hairline border +
/// a top-edge highlight that fades down the sides (light catching a bezel).
/// WHY a gradient pen for the highlight: one stroke gives the top edge the
/// brightest line and lets it die out down the sides, which reads as depth
/// without extra draw calls.
inline void draw_panel(QPainter& p, QRectF const& r) noexcept {
    draw_shadow(p, r, kPanelRadius);

    QLinearGradient fill(r.topLeft(), r.bottomLeft());
    fill.setColorAt(0.0, kPanelTop);
    fill.setColorAt(1.0, kPanelBottom);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, kPanelRadius, kPanelRadius);

    // Hairline border.
    p.setPen(QPen(QBrush(kBorder), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), kPanelRadius,
                      kPanelRadius);

    // Top-edge highlight fading down the sides.
    QLinearGradient hl(r.topLeft(), QPointF(r.left(), r.bottom()));
    hl.setColorAt(0.0, QColor(255, 255, 255, 26));
    hl.setColorAt(0.12, QColor(255, 255, 255, 8));
    hl.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setPen(QPen(QBrush(hl), 1.0));
    p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), kPanelRadius - 1,
                      kPanelRadius - 1);
}

/// Inset "well": recessed area for meters, knobs, pads, sliders.
inline void draw_well(QPainter& p, QRectF const& r, double radius = 8.0) noexcept {
    p.setPen(QPen(QColor(0, 0, 0, 110), 1.0));
    QLinearGradient fill(r.topLeft(), r.bottomLeft());
    fill.setColorAt(0.0, QColor(0x12, 0x14, 0x19));
    fill.setColorAt(1.0, kWell);
    p.setBrush(fill);
    p.drawRoundedRect(r, radius, radius);
    // Faint bottom-edge light bounce to sell the inset.
    QLinearGradient bl(r.topLeft(), QPointF(r.left(), r.bottom()));
    bl.setColorAt(0.0, QColor(255, 255, 255, 0));
    bl.setColorAt(1.0, QColor(255, 255, 255, 10));
    p.setPen(QPen(QBrush(bl), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
}

}  // namespace addu94::ui::theme
