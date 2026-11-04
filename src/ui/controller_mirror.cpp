// addu94 — src/ui/controller_mirror.cpp
#include "ui/controller_mirror.hpp"

#include "ui/theme.hpp"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QRadialGradient>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace addu94::ui {

namespace {

using theme::kAccent;
using theme::kAccentHot;
using theme::kBorder;
using theme::kGreen;
using theme::kMuted;
using theme::kText;
using theme::with_alpha;

constexpr int kKnobSweep = 270;          // degrees of knob travel
constexpr std::int64_t kPulseMs = 240;   // pad glow decay (ease-out)
constexpr std::int64_t kKnobGlowMs = 200;  // knob "just moved" glow
constexpr int kPadCount = 16;            // 2 banks x 8
constexpr int kMaxPacks = 16;            // chip cache ceiling

// Knob action labels per mode. KEEP IN SYNC with map_cc() in
// src/mapping/mapping.cpp: play_actions[] / edit_actions[] there must match
// this table (order AND meaning); STEP mode uses knob 1 = step velocity,
// the rest are no-ops in M3.
constexpr std::array<char const*, 8> kPlayLabels{
    "vol", "pan", "send", "macro", "cut", "res", "atk", "rel"};
constexpr std::array<char const*, 8> kEditLabels{
    "scrubC", "scrubF", "tempo", "loop", "quant", "master", "click", "-"};
constexpr std::array<char const*, 8> kStepLabels{
    "vel", "-", "-", "-", "-", "-", "-", "-"};

// Per-pad hue tints. Bank A (drum kit family): kick warm red, snare cool
// steel, clap violet, hats brass, toms amber, perc teal, ride sage — each
// pad gets its own identity so hits are readable per voice. Bank B: undo/
// redo teal pair, chord slots in a violet ramp.
constexpr int kPadHue[16] = {12,  205, 285, 55,  20,  35,  160, 95,
                             190, 190, 265, 272, 279, 286, 293, 300};

std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ControllerMirror::ControllerMirror(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip("Live mirror of the MPK Mini — knobs (with resolved CC), pads "
               "(both banks), mode, pad pack and track. Click PLAY / EDIT / "
               "STEP, a pack chip or a track chip.");
    setMinimumHeight(224);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ControllerMirror::pulse_pad(int pad_index) {
    if (pad_index >= 0 && pad_index < kPadCount) {
        pad_hit_ms_[static_cast<std::size_t>(pad_index)] = steady_ms();
        update();
    }
}

void ControllerMirror::refresh() { update(); }

void ControllerMirror::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    now_ms_ = steady_ms();

    QRectF const full = QRectF(rect());

    // Panel: glass background, rounded corners, hairline border.
    theme::draw_panel(p, full);

    // Layout: knobs strip on top, then one full-width row with the pad-pack
    // chips (left) and track-role chips (right), then pads (left) and the
    // mode/status column (right). WHY a dedicated row: pack + track
    // selection are the two global "where does input land" choices, so they
    // get full width instead of being squeezed into the status column.
    double const pad = 8.0;
    QRectF const inner = full.adjusted(pad, pad, -pad, -pad);
    // 0.42 of the inner height leaves room for the three text rows under
    // each knob (value / label / CC) without bleeding into the chip row.
    double const knobs_h = inner.height() * 0.42;
    double const rows_h = 26.0;
    QRectF const knobs{inner.left(), inner.top(), inner.width(), knobs_h};
    double const rows_y = knobs.bottom() + 6.0;
    QRectF const rows{inner.left(), rows_y, inner.width(), rows_h};
    double const bottom_y = rows.bottom() + 6.0;
    double const pad_w = 4.0 * 44.0 + 3.0 * 4.0 + 8.0;  // 4x4 cells + gaps
    QRectF const pads{inner.left(), bottom_y, pad_w, inner.bottom() - bottom_y};
    QRectF const status{pads.right() + 12.0, bottom_y,
                        inner.right() - pads.right() - 12.0, pads.height()};

    theme::draw_well(p, knobs, 8);
    theme::draw_well(p, pads, 8);
    theme::draw_well(p, status, 8);

    paint_knobs(p, knobs);
    paint_packs_tracks(p, rows);
    paint_pads(p, pads);
    paint_status(p, status);
}

void ControllerMirror::paint_knobs(QPainter& p, QRectF const& area) {
    auto const mode = engine_.mapping().mode();
    std::array<char const*, 8> const& labels =
        mode == mapping::Mode::Edit
            ? kEditLabels
            : (mode == mapping::Mode::Step ? kStepLabels : kPlayLabels);

    QFont const value_font = theme::value_font(font(), 9);
    QFont const small{font().family(), std::max(font().pointSize() - 2, 7)};
    QFont const micro = theme::label_font(font());

    double const cell_w = area.width() / 8.0;
    // Keep cy + r ≤ height - 36 so the three text rows under the knob
    // (value / action / CC) always fit inside the strip.
    double const cy = area.top() + area.height() * 0.32;
    double const r = std::min(cell_w, area.height()) * 0.26;

    for (int i = 0; i < 8; ++i) {
        double const cx = area.left() + cell_w * (i + 0.5);
        QRectF const arc_box{cx - r, cy - r, 2 * r, 2 * r};

        int const v = engine_.mapping().knob_value(i);
        double const frac = static_cast<double>(v) / 127.0;

        // Knob body: recessed inner disc with a soft radial falloff so the
        // ring reads as a physical cap.
        QRadialGradient body{arc_box.center(), r};
        body.setColorAt(0.0, QColor(0x2d, 0x32, 0x3c));
        body.setColorAt(0.75, QColor(0x22, 0x26, 0x2e));
        body.setColorAt(1.0, QColor(0x18, 0x1b, 0x21));
        p.setPen(QPen(QColor(0, 0, 0, 120), 1.0));
        p.setBrush(body);
        p.drawEllipse(arc_box.center(), r * 0.62, r * 0.62);

        // Base ring: recessed 270° track starting at 225° (Qt angles: 1/16
        // deg, counter-clockwise positive).
        p.setPen(QPen(QColor(0x39, 0x3f, 0x4a), 3.5, Qt::SolidLine,
                      Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(arc_box, 225 * 16, -kKnobSweep * 16);

        // "Just moved" glow: wide translucent accent ring under the value
        // arc, fading over ~200 ms. WHY: at 30 Hz a single bright frame is
        // invisible; the short decay makes turns feel tactile.
        int const start16 = 225 * 16;
        int const sweep16 =
            -static_cast<int>(static_cast<double>(kKnobSweep) * frac) * 16;
        if (knob_last_[static_cast<std::size_t>(i)] != v) {
            knob_last_[static_cast<std::size_t>(i)] = v;
            knob_move_ms_[static_cast<std::size_t>(i)] = now_ms_;
        }
        double const age =
            static_cast<double>(now_ms_ -
                                knob_move_ms_[static_cast<std::size_t>(i)]) /
            static_cast<double>(kKnobGlowMs);
        if (frac > 0.0 && age < 1.0) {
            double const a = (1.0 - age) * 0.45;
            p.setPen(QPen(with_alpha(kAccent, a), 6.5, Qt::SolidLine,
                          Qt::RoundCap));
            p.drawArc(arc_box, start16, sweep16);
        }

        // Value arc: accent with a hot tip (short brighter cap at the end).
        if (frac > 0.0) {
            p.setPen(QPen(kAccent, 3.5, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_box, start16, sweep16);
            p.setPen(QPen(kAccentHot, 3.5, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_box, start16 + sweep16, -6 * 16);
        }

        // Pointer line on the body, toward the value angle.
        double const angle_deg = 225.0 - kKnobSweep * frac;
        double const rad = angle_deg * 3.14159265358979323846 / 180.0;
        p.setPen(QPen(kText, 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF{cx + r * 0.16 * std::cos(rad),
                           cy - r * 0.16 * std::sin(rad)},
                   QPointF{cx + r * 0.48 * std::cos(rad),
                           cy - r * 0.48 * std::sin(rad)});

        // Value, action label, resolved CC number (micro, muted) — three
        // calm rows under the knob.
        p.setPen(kText);
        p.setFont(value_font);
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 1, cell_w, 13},
                   Qt::AlignHCenter | Qt::AlignTop, QString::number(v));
        p.setPen(kMuted);
        p.setFont(small);
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 14, cell_w, 11},
                   Qt::AlignHCenter | Qt::AlignTop,
                   QString(labels[static_cast<std::size_t>(i)]));
        p.setFont(micro);
        p.setPen(with_alpha(kMuted, 0.75));
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 25, cell_w, 10},
                   Qt::AlignHCenter | Qt::AlignTop,
                   QString("cc %1").arg(engine_.mapping().knob_cc(i)));
    }
}

void ControllerMirror::paint_chip(QPainter& p, QRectF const& r,
                                  QString const& text, bool on,
                                  QColor const& hue, QFont const& chip_font) {
    if (on) {
        QLinearGradient fill{r.topLeft(), r.bottomLeft()};
        fill.setColorAt(0.0, with_alpha(kAccent, 0.34));
        fill.setColorAt(1.0, with_alpha(kAccent, 0.16));
        p.setPen(QPen(kAccent, 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);
        p.setPen(kText);
    } else {
        QLinearGradient fill{r.topLeft(), r.bottomLeft()};
        fill.setColorAt(0.0, QColor(0x24, 0x28, 0x30));
        fill.setColorAt(1.0, QColor(0x1b, 0x1e, 0x24));
        p.setPen(QPen(kBorder, 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);
        p.setPen(kMuted);
    }
    p.setFont(chip_font);
    p.drawText(r, Qt::AlignCenter, text);

    // Hue dot (identity marker) on the left edge — quiet unless `on`.
    double const dot_r = 2.5;
    QColor const dot = on ? hue : with_alpha(hue, 0.55);
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(QPointF{r.left() + 7.0, r.center().y()}, dot_r, dot_r);
}

void ControllerMirror::paint_packs_tracks(QPainter& p, QRectF const& area) {
    QFont const chip_font{font().family(), std::max(font().pointSize() - 2, 7)};
    QFont const tag_font = theme::label_font(font());

    double const chip_h = 20.0;
    double const cy = area.top() + (area.height() - chip_h) / 2.0;

    // --- pad-pack chips (left half) ---
    p.setPen(kMuted);
    p.setFont(tag_font);
    p.drawText(QRectF{area.left(), cy, 44.0, chip_h},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("pack"));
    QFontMetrics const fm{chip_font};
    double const chip_gap = 5.0;
    double const x0 = area.left() + 50.0;
    double const half = area.width() * 0.52;
    int const n = std::min<int>(
        static_cast<int>(engine_.pad_packs().size()), kMaxPacks);
    double x = x0;
    pack_count_ = 0;
    for (int i = 0; i < n && x < area.right() - 40.0; ++i) {
        auto const& info = engine_.pad_packs()[static_cast<std::size_t>(i)];
        double const w =
            std::min(fm.horizontalAdvance(QString::fromStdString(info.name)) +
                         20.0,
                     x0 + half - x);
        QRectF const chip{x, cy, w, chip_h};
        bool const on = engine_.current_pad_pack() == info.index;
        paint_chip(p, chip, QString::fromStdString(info.name), on, kAccent,
                   chip_font);
        pack_rects_[static_cast<std::size_t>(pack_count_)] = chip;
        pack_index_[static_cast<std::size_t>(pack_count_)] = info.index;
        ++pack_count_;
        x += w + chip_gap;
    }

    // --- track-role chips (right half) ---
    double const tx = area.left() + area.width() * 0.55;
    p.setPen(kMuted);
    p.setFont(tag_font);
    p.drawText(QRectF{tx, cy, 40.0, chip_h},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("track"));
    double const tw = (area.right() - (tx + 44.0) - 3 * 5.0) / 4.0;
    for (int i = 0; i < 4; ++i) {
        QRectF const chip{tx + 44.0 + static_cast<double>(i) * (tw + 5.0), cy,
                          tw, chip_h};
        bool const on = engine_.selected_track() == i;
        QColor const hue = theme::role_color(
            static_cast<int>(engine_.track_role(i)));
        paint_chip(p, chip, QString::fromUtf8(engine_.track_name(i)), on, hue,
                   chip_font);
        track_rects_[static_cast<std::size_t>(i)] = chip;
    }
}

void ControllerMirror::paint_pads(QPainter& p, QRectF const& area) {
    bool const bank_b = engine_.mapping().pad_bank_b();
    QFont const pad_font{font().family(), std::max(font().pointSize() - 2, 6)};

    double const gap = 4.0;
    double const cell_w = (area.width() - 3 * gap) / 4.0;
    double const cell_h = (area.height() - 3 * gap) / 4.0;

    for (int idx = 0; idx < kPadCount; ++idx) {
        // Columns 0-1: bank A pads A1..A8 (row-major); columns 2-3: B1..B8.
        int const local = idx % 8;
        int const col_base = idx < 8 ? 0 : 2;
        int const row = local / 2;
        int const col = col_base + local % 2;

        QRectF const cell{area.left() + col * (cell_w + gap),
                          area.top() + row * (cell_h + gap), cell_w, cell_h};

        bool const active_bank = (idx < 8) != bank_b;
        int const hue = kPadHue[static_cast<std::size_t>(idx)];

        // Resting state: dark pad with the per-pad hue tint so voices stay
        // identifiable even before a hit.
        QColor fill = QColor::fromHsl(hue, 100, 48);
        QColor border = active_bank ? kAccent : kBorder;

        // Hit glow: ease-out decay over kPulseMs — a fast bright flash that
        // lingers just long enough to feel like a real rubber pad lighting
        // up (linear felt dead; the power curve keeps the tail visible).
        std::int64_t const hit = pad_hit_ms_[static_cast<std::size_t>(idx)];
        if (hit != 0) {
            double const age =
                static_cast<double>(now_ms_ - hit) / static_cast<double>(kPulseMs);
            if (age < 1.0) {
                double const a = std::pow(1.0 - age, 1.6);
                fill = QColor::fromHsl(hue, 190, 150);
                fill.setAlphaF(static_cast<float>(0.25 + 0.75 * a));
                // Soft halo while the glow is fresh.
                if (a > 0.2) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(with_alpha(QColor::fromHsl(hue, 190, 150),
                                          0.22 * a));
                    p.drawRoundedRect(cell.adjusted(-3.5, -3.5, 3.5, 3.5), 8, 8);
                }
                border = with_alpha(kText, 0.4 + 0.6 * a);
            }
        }

        p.setPen(QPen(border, active_bank ? 1.4 : 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(cell, 4, 4);
        // Top-edge inner highlight (pad "rubber" sheen).
        p.setPen(QPen(QColor(255, 255, 255, 14), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(cell.adjusted(1, 1, -1, -1), 3, 3);

        p.setPen(active_bank ? kText : with_alpha(kText, 0.65));
        p.setFont(pad_font);
        QString const label =
            QString("%1%2").arg(idx < 8 ? QLatin1Char('A') : QLatin1Char('B'))
                .arg(local + 1);
        p.drawText(cell, Qt::AlignCenter, label);
    }
}

void ControllerMirror::paint_status(QPainter& p, QRectF const& area) {
    auto const mode = engine_.mapping().mode();
    char const* mode_names[3] = {"PLAY", "EDIT", "STEP"};
    int const mode_idx = mode == mapping::Mode::Edit
                             ? 1
                             : (mode == mapping::Mode::Step ? 2 : 0);

    QFont const seg_font{font().family(), std::max(font().pointSize() - 1, 7)};
    QFont const tiny{font().family(), std::max(font().pointSize() - 2, 6)};

    // Mode strip: sleek segmented control — recessed container, the active
    // segment filled with the accent gradient and a soft outer glow.
    double const seg_h = 26.0;
    double const seg_gap = 3.0;
    double const seg_w = (area.width() - 2 * seg_gap) / 3.0;

    QRectF const container{area.left(), area.top() + 5.0, area.width(), seg_h};
    theme::draw_well(p, container, 7);

    for (int i = 0; i < 3; ++i) {
        QRectF const seg{container.left() + i * (seg_w + seg_gap) + 1.5,
                         container.top() + 1.5, seg_w - 3.0, seg_h - 3.0};
        mode_rects_[i] = seg;
        bool const on = i == mode_idx;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(kAccent, 0.14));
            p.drawRoundedRect(seg.adjusted(-2.0, -2.0, 2.0, 2.0), 7, 7);
            QLinearGradient fill{seg.topLeft(), seg.bottomLeft()};
            fill.setColorAt(0.0, kAccentHot);
            fill.setColorAt(1.0, kAccent);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(seg, 5, 5);
            p.setPen(QColor(0x0b, 0x12, 0x1b));
        } else {
            p.setPen(kMuted);
        }
        p.setFont(seg_font);
        p.drawText(seg, Qt::AlignCenter, QString(mode_names[i]));
    }

    // Pad legend (word-wrapped, tiny) + octave readout.
    double const info_y = container.bottom() + 7.0;
    int const oct = engine_.octave();
    QString const oct_text = QString("OCT %1%2")
                                 .arg(oct >= 0 ? QLatin1Char('+') : QLatin1Char('-'))
                                 .arg(oct < 0 ? -oct : oct);
    p.setFont(tiny);
    p.setPen(kMuted);
    QRectF const legend{area.left(), info_y, area.width() * 0.70,
                        area.bottom() - info_y};
    p.drawText(legend,
               Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
               QString("A drums · B1/B2 undo/redo · B3-B8 chords (hold+keys=capture)"));
    p.setPen(kGreen);
    p.setFont(theme::value_font(font(), 9));
    p.drawText(QRectF{area.right() - area.width() * 0.28, info_y,
                      area.width() * 0.28, 16},
               Qt::AlignRight | Qt::AlignTop, oct_text);
}

void ControllerMirror::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    // Pad-pack chips → engine_.set_pad_pack(i) (app-thread API; we are on
    // the UI thread, the only thread allowed to call it).
    for (int i = 0; i < pack_count_; ++i) {
        if (pack_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            std::string error;
            engine_.set_pad_pack(pack_index_[static_cast<std::size_t>(i)],
                                 error);
            update();
            return;
        }
    }
    // Track-role chips → select the track (keys/pads follow it).
    for (int i = 0; i < 4; ++i) {
        if (track_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            engine_.select_track(i);
            update();
            return;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (mode_rects_[i].contains(pos)) {
            auto const m = static_cast<mapping::Mode>(i);  // 0=Play 1=Edit 2=Step
            engine_.mapping().set_mode(m);
            update();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

}  // namespace addu94::ui
