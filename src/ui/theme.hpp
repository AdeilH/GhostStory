// ghost_story — src/ui/theme.hpp
//
// Shared visual language for the UI ("one screen = the instrument").
// Header-only so every custom-painted widget stays in sync without a
// translation unit.
//
// THEMES. One Palette struct, four hand-tuned instances. The active
// palette is process-global (set_theme); widgets read it through pal()
// at paint time and repaint on change (generation() ticks on every
// switch — MainWindow/HealthStrip compare it in their 30 Hz tick and
// refresh stylesheets only when it moved, so switching costs one frame,
// not a busy loop).
//
//   Graphite  — the default: neutral charcoal surfaces, near-flat
//               panels, a restrained steel accent. Hardware, not neon.
//   Midnight  — the original launch look: deep blue-black glass with
//               the electric-blue signature accent.
//   Slate     — cool blue-gray surfaces with a calm teal accent.
//   Paper     — light warm theme: paper surfaces, ink text, terracotta
//               accent. Flat on purpose (light surfaces don't glow).
//
// The look rules that keep every theme professional rather than
// "vibe-coded": surfaces are quiet (surfaces never out-shout text), one
// accent carries selection/motion, track colors are desaturated enough
// to sit behind content, and gradients/shadows/glow are knobs the
// palette turns (Paper sets highlight_alpha 0 = flat, not a separate
// drawing path).
//
// Typography has three tiers, used consistently everywhere:
//   labels  — tiny, uppercase, letter-spaced, muted   (what is this?)
//   values  — demi-bold, one step larger, full text   (what does it say?)
//   hints   — tiny regular, muted                     (how do I use it?)
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
#include <QString>

#include <algorithm>
#include <atomic>

namespace ghost_story::ui::theme {

// Panels / geometry (theme-independent).
inline constexpr int kPanelRadius = 12;   // rounded corner radius
inline constexpr int kPanelPad = 8;       // inner padding
inline constexpr int kPanelGap = 12;      // space between panels

// --- Palette -------------------------------------------------------------
struct Palette {
    char const* name;
    QColor base_top;      // window gradient top
    QColor base_bottom;   // window gradient bottom
    QColor panel_top;     // panel gradient top
    QColor panel_bottom;  // panel gradient bottom
    QColor border;        // hairline border
    QColor well;          // inset track/arc bg
    QColor grid;          // faint grid lines
    QColor accent;        // signature accent (selection, motion, arcs)
    QColor accent_hot;    // bright tip of arcs / glow
    QColor record;        // record arm / destructive red
    QColor text;          // primary text
    QColor muted;         // secondary text
    QColor green;         // healthy/OK green
    int track_hue[4];     // T1..T4 hues (degrees)
    int track_sat;        // track_color() HSL saturation (0..255)
    int track_light;      // track_color() HSL lightness (0..255)
    int shadow_alpha;     // panel drop-shadow strength (0..255)
    int highlight_alpha;  // top-edge bezel highlight (0 = flat)
};

// The four themes. Colors are picked by hand: surfaces first (quiet,
// near-neutral), then one accent, then track hues tuned to sit behind
// content. See the file comment for the design rules.
inline const Palette kThemes[4] = {
    {/* .name = */ "Graphite",
     /* base_top = */ QColor{0x17, 0x18, 0x1b},
     /* base_bottom = */ QColor{0x1b, 0x1c, 0x1f},
     /* panel_top = */ QColor{0x23, 0x24, 0x27},
     /* panel_bottom = */ QColor{0x1f, 0x20, 0x23},
     /* border = */ QColor{0x31, 0x33, 0x36},
     /* well = */ QColor{0x15, 0x16, 0x19},
     /* grid = */ QColor{0x26, 0x28, 0x2b},
     /* accent = */ QColor{0x8f, 0xa6, 0xbd},
     /* accent_hot = */ QColor{0xcc, 0xd9, 0xe6},
     /* record = */ QColor{0xd9, 0x6a, 0x5f},
     /* text = */ QColor{0xd8, 0xdb, 0xdf},
     /* muted = */ QColor{0x8f, 0x95, 0x9e},
     /* green = */ QColor{0x86, 0xb9, 0x8f},
     /* track_hue = */ {210, 160, 25, 290},
     /* track_sat = */ 105, /* track_light = */ 150,
     /* shadow_alpha = */ 55,
     /* highlight_alpha = */ 12},
    {/* .name = */ "Midnight",
     /* base_top = */ QColor{0x0d, 0x0f, 0x13},
     /* base_bottom = */ QColor{0x15, 0x17, 0x1c},
     /* panel_top = */ QColor{0x21, 0x25, 0x2d},
     /* panel_bottom = */ QColor{0x19, 0x1c, 0x22},
     /* border = */ QColor{0x2c, 0x31, 0x3b},
     /* well = */ QColor{0x14, 0x16, 0x1b},
     /* grid = */ QColor{0x22, 0x26, 0x2e},
     /* accent = */ QColor{0x4f, 0x9d, 0xff},
     /* accent_hot = */ QColor{0x9c, 0xcb, 0xff},
     /* record = */ QColor{0xff, 0x5a, 0x5a},
     /* text = */ QColor{0xe9, 0xed, 0xf3},
     /* muted = */ QColor{0x8a, 0x93, 0xa3},
     /* green = */ QColor{0x3e, 0xcf, 0x8e},
     /* track_hue = */ {212, 165, 25, 292},
     /* track_sat = */ 158, /* track_light = */ 152,
     /* shadow_alpha = */ 85,
     /* highlight_alpha = */ 26},
    {/* .name = */ "Slate",
     /* base_top = */ QColor{0x10, 0x15, 0x1a},
     /* base_bottom = */ QColor{0x14, 0x1a, 0x20},
     /* panel_top = */ QColor{0x1c, 0x23, 0x2b},
     /* panel_bottom = */ QColor{0x18, 0x1f, 0x26},
     /* border = */ QColor{0x2e, 0x39, 0x44},
     /* well = */ QColor{0x0e, 0x13, 0x19},
     /* grid = */ QColor{0x1f, 0x28, 0x30},
     /* accent = */ QColor{0x4a, 0xa8, 0x96},
     /* accent_hot = */ QColor{0xa0, 0xd8, 0xcb},
     /* record = */ QColor{0xe0, 0x68, 0x68},
     /* text = */ QColor{0xdc, 0xe3, 0xe9},
     /* muted = */ QColor{0x86, 0x92, 0x9e},
     /* green = */ QColor{0x6c, 0xb9, 0x8a},
     /* track_hue = */ {205, 160, 28, 285},
     /* track_sat = */ 110, /* track_light = */ 145,
     /* shadow_alpha = */ 65,
     /* highlight_alpha = */ 16},
    {/* .name = */ "Paper",
     /* base_top = */ QColor{0xec, 0xea, 0xe4},
     /* base_bottom = */ QColor{0xe5, 0xe3, 0xdc},
     /* panel_top = */ QColor{0xf6, 0xf4, 0xef},
     /* panel_bottom = */ QColor{0xef, 0xed, 0xe7},
     /* border = */ QColor{0xcc, 0xc8, 0xbd},
     /* well = */ QColor{0xe2, 0xdf, 0xd7},
     /* grid = */ QColor{0xdb, 0xd7, 0xcd},
     /* accent = */ QColor{0xb0, 0x55, 0x2e},
     /* accent_hot = */ QColor{0x7c, 0x3d, 0x20},  // darker reads "hot" on light
     /* record = */ QColor{0xb2, 0x3c, 0x34},
     /* text = */ QColor{0x2a, 0x28, 0x23},
     /* muted = */ QColor{0x77, 0x72, 0x6a},
     /* green = */ QColor{0x4d, 0x8a, 0x5d},
     /* track_hue = */ {210, 160, 25, 290},
     /* track_sat = */ 118, /* track_light = */ 104,
     /* shadow_alpha = */ 30,
     /* highlight_alpha = */ 0},  // flat: light surfaces don't glow
};

inline constexpr int kThemeCount = 4;

/// Active palette index (process-global; set from QSettings at startup
/// and by the THEME combo). Atomic so the RT-adjacent UI tick can poll
/// it without locks.
inline std::atomic<int> g_theme{0};
inline std::atomic<int> g_generation{0};  // bumped on every set_theme

[[nodiscard]] inline int theme_index() noexcept {
    return g_theme.load(std::memory_order_relaxed);
}
[[nodiscard]] inline int generation() noexcept {
    return g_generation.load(std::memory_order_relaxed);
}
inline void set_theme(int index) noexcept {
    g_theme.store(index < 0 ? 0 : index % kThemeCount,
                  std::memory_order_relaxed);
    g_generation.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] inline char const* theme_name(int index) noexcept {
    return kThemes[index < 0 ? 0 : index % kThemeCount].name;
}
/// The active palette. Call at paint time — never cache QColors across
/// a theme switch.
[[nodiscard]] inline Palette const& pal() noexcept {
    return kThemes[theme_index()];
}
/// True when the palette is light (dark text on light surfaces). Widgets
/// use it to flip white/black tints that can't be palette entries.
[[nodiscard]] inline bool is_light() noexcept {
    return pal().text.lightness() < 128;
}

// --- color helpers -------------------------------------------------------
[[nodiscard]] inline QColor with_alpha(QColor const& c, double a) noexcept {
    QColor out = c;
    out.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return out;
}

/// HSL lightness shift (negative darkens). Surfaces that used to be
/// hardcoded tints ("a touch lighter than the panel") derive from the
/// palette this way, so every theme keeps its own micro-contrast.
[[nodiscard]] inline QColor shift(QColor const& c, int dl) noexcept {
    QColor out = c;
    out.setHsl(c.hslHue() < 0 ? 0 : c.hslHue(),
               std::clamp(c.hslSaturation() + (dl > 0 ? 8 : 0), 0, 255),
               std::clamp(c.lightness() + dl, 0, 255));
    return out;
}

// --- Qt-widget styling ----------------------------------------------------
/// Stylesheet for the app's QComboBox dropdowns (theme, velocity, pack,
/// preset). One source so every popup speaks the same visual language and
/// a theme switch re-tints them all (callers re-apply when generation()
/// moves). WHY explicit ::item rules: without them the popup rows keep
/// the style's default compact metrics and the selection highlight only
/// half-applies — the list reads as blank lines instead of pickable
/// themes, which is exactly what made the picker feel invisible.
[[nodiscard]] inline QString combo_stylesheet() noexcept {
    // Light themes select with panel-tone text on the accent; dark themes
    // invert onto the near-black base so the accent stays the only glow.
    QString const selected_text =
        is_light() ? pal().panel_top.name() : pal().base_top.name();
    return QStringLiteral(
               "QComboBox { background: %1; color: %2;"
               "  border: 1px solid %3; border-radius: 6px; padding: 0 8px; }"
               "QComboBox:hover { border-color: %4; }"
               "QComboBox QAbstractItemView { background: %1; color: %2;"
               "  border: 1px solid %3; selection-background-color: %4;"
               "  selection-color: %5; outline: 0; }"
               "QComboBox QAbstractItemView::item { min-height: 24px;"
               "  padding: 2px 10px; }"
               "QComboBox QAbstractItemView::item:selected {"
               "  background: %4; color: %5; }")
        .arg(shift(pal().panel_top, -3).name(), pal().text.name(),
             pal().border.name(), pal().accent.name(), selected_text);
}

[[nodiscard]] inline QColor track_color(int track) noexcept {
    Palette const& p = pal();
    return QColor::fromHsl(p.track_hue[track % 4], p.track_sat,
                           p.track_light);
}

// Track-kind labels/hues, indexed by app::TrackKind's underlying value
// (0=Synth 1=Drums). Kept as plain ints so this header does not have to
// pull in play_engine.hpp. WHY the UI should speak kinds: the mapping
// routes keys/pads per kind, so labeling rows by kind (not just name)
// tells the player where input will land. Per-slot identity color comes
// from track_color(slot) — kind_color only separates the two kinds.
[[nodiscard]] inline char const* kind_label(int kind) noexcept {
    return kind == 1 ? "DRUM KIT" : "SYNTH";
}

[[nodiscard]] inline QColor kind_color(int kind) noexcept {
    Palette const& p = pal();
    return kind == 1 ? QColor::fromHsl(p.track_hue[2], p.track_sat,
                                       p.track_light)
                     : QColor::fromHsl(p.track_hue[0], p.track_sat,
                                       p.track_light);
}

// --- typography tiers ---------------------------------------------------
/// Tiny uppercase tracked-out label (what is this?).
[[nodiscard]] inline QFont label_font(QFont const& base) noexcept {
    QFont f = base;
    f.setPointSize(std::max(base.pointSize() - 3, 6));
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
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
    p.setBrush(QColor(0, 0, 0, pal().shadow_alpha));
    p.drawRoundedRect(r.translated(0.0, 3.0), radius, radius);
}

/// Panel: soft shadow + (optionally graded) fill + hairline border + a
/// top-edge highlight that fades down the sides (light catching a bezel).
/// WHY a gradient pen for the highlight: one stroke gives the top edge the
/// brightest line and lets it die out down the sides, which reads as depth
/// without extra draw calls. Palettes turn the gradient and highlight off
/// (flat themes) through highlight_alpha — no extra paths.
inline void draw_panel(QPainter& p, QRectF const& r) noexcept {
    Palette const& th = pal();
    draw_shadow(p, r, kPanelRadius);

    QLinearGradient fill(r.topLeft(), r.bottomLeft());
    fill.setColorAt(0.0, th.panel_top);
    fill.setColorAt(1.0, th.panel_bottom);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, kPanelRadius, kPanelRadius);

    // Hairline border.
    p.setPen(QPen(QBrush(th.border), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), kPanelRadius,
                      kPanelRadius);

    // Top-edge highlight fading down the sides (flat themes: skip).
    if (th.highlight_alpha > 0) {
        QLinearGradient hl(r.topLeft(), QPointF(r.left(), r.bottom()));
        hl.setColorAt(0.0, QColor(255, 255, 255, th.highlight_alpha));
        hl.setColorAt(0.12, QColor(255, 255, 255, th.highlight_alpha / 3));
        hl.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setPen(QPen(QBrush(hl), 1.0));
        p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), kPanelRadius - 1,
                          kPanelRadius - 1);
    }
}

/// Inset "well": recessed area for meters, knobs, pads, sliders.
inline void draw_well(QPainter& p, QRectF const& r, double radius = 8.0) noexcept {
    Palette const& th = pal();
    p.setPen(QPen(is_light() ? with_alpha(th.border, 1.0)
                             : QColor(0, 0, 0, 110), 1.0));
    QLinearGradient fill(r.topLeft(), r.bottomLeft());
    fill.setColorAt(0.0, shift(th.well, 4));
    fill.setColorAt(1.0, th.well);
    p.setBrush(fill);
    p.drawRoundedRect(r, radius, radius);
    // Faint edge bounce to sell the inset: light from above means a
    // brighter floor on dark themes, a darker one on light themes.
    bool const light = is_light();
    QLinearGradient bl(r.topLeft(), QPointF(r.left(), r.bottom()));
    bl.setColorAt(0.0, light ? QColor(0, 0, 0, 0) : QColor(255, 255, 255, 0));
    bl.setColorAt(1.0, light ? QColor(0, 0, 0, 22) : QColor(255, 255, 255, 10));
    p.setPen(QPen(QBrush(bl), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), radius - 1, radius - 1);
}

}  // namespace ghost_story::ui::theme
