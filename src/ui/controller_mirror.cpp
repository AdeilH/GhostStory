// addu94 — src/ui/controller_mirror.cpp
#include "ui/controller_mirror.hpp"

#include "ui/theme.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <chrono>

namespace addu94::ui {

namespace {

using theme::kAccent;
using theme::kBorder;
using theme::kGreen;
using theme::kMuted;
using theme::kText;
using theme::with_alpha;

constexpr int kKnobSweep = 270;          // degrees of knob travel
constexpr std::int64_t kPulseMs = 150;   // pad glow decay
constexpr int kPadCount = 16;            // 2 banks x 8

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

std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ControllerMirror::ControllerMirror(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip("Live mirror of the MPK Mini — knobs, pads (both banks) and "
               "mode. Click PLAY / EDIT / STEP to switch modes.");
    setMinimumHeight(170);
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

    // Panel: theme background, rounded corners, subtle border.
    theme::draw_panel(p, full);

    // Top strip: 8 knobs. Bottom: pads on the left, mode/status on the right.
    double const pad = 8.0;
    QRectF const inner = full.adjusted(pad, pad, -pad, -pad);
    double const knobs_h = inner.height() * 0.44;
    QRectF const knobs{inner.left(), inner.top(), inner.width(), knobs_h};
    double const bottom_y = knobs.bottom() + 8.0;
    double const pad_w = 4.0 * 44.0 + 3.0 * 4.0 + 8.0;  // 4x4 cells + gaps
    QRectF const pads{inner.left(), bottom_y, pad_w, inner.bottom() - bottom_y};
    QRectF const status{pads.right() + 12.0, bottom_y,
                        inner.right() - pads.right() - 12.0, pads.height()};

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x17, 0x1a, 0x1f));
    p.drawRoundedRect(knobs, 8, 8);
    p.drawRoundedRect(pads, 8, 8);
    p.drawRoundedRect(status, 8, 8);

    paint_knobs(p, knobs);
    paint_pads(p, pads);
    paint_status(p, status);
}

void ControllerMirror::paint_knobs(QPainter& p, QRectF const& area) {
    auto const mode = engine_.mapping().mode();
    std::array<char const*, 8> const& labels =
        mode == mapping::Mode::Edit
            ? kEditLabels
            : (mode == mapping::Mode::Step ? kStepLabels : kPlayLabels);

    QFont const small = font();
    QFont const tiny{small.family(), std::max(small.pointSize() - 3, 6)};

    double const cell_w = area.width() / 8.0;
    double const cy = area.top() + area.height() * 0.38;
    double const r = std::min(cell_w, area.height()) * 0.34;

    for (int i = 0; i < 8; ++i) {
        double const cx = area.left() + cell_w * (i + 0.5);
        QRectF const arc_box{cx - r, cy - r, 2 * r, 2 * r};

        // Base arc: 270° sweep starting at 225° (Qt angles: 1/16 deg,
        // counter-clockwise positive).
        QPen base_pen(kBorder, 3.0, Qt::SolidLine, Qt::RoundCap);
        p.setPen(base_pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(arc_box, 225 * 16, -kKnobSweep * 16);

        // Value arc (accent).
        int const v = engine_.mapping().knob_value(i);
        double const frac = static_cast<double>(v) / 127.0;
        if (frac > 0.0) {
            p.setPen(QPen(kAccent, 3.0, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_box, 225 * 16,
                      -static_cast<int>(static_cast<double>(kKnobSweep) * frac) * 16);
        }

        // Pointer dot at the value angle.
        double const angle_deg = 225.0 - kKnobSweep * frac;
        double const rad = angle_deg * 3.14159265358979323846 / 180.0;
        p.setPen(Qt::NoPen);
        p.setBrush(kText);
        double const dot_r = r * 0.22;
        p.drawEllipse(QPointF{cx + r * 0.62 * std::cos(rad),
                              cy - r * 0.62 * std::sin(rad)},
                      dot_r, dot_r);

        p.setPen(kText);
        p.setFont(small);
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 1, cell_w, 14},
                   Qt::AlignHCenter | Qt::AlignTop, QString::number(v));
        p.setPen(kMuted);
        p.setFont(tiny);
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 15, cell_w, 12},
                   Qt::AlignHCenter | Qt::AlignTop,
                   QString(labels[static_cast<std::size_t>(i)]));
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
        QColor fill = QColor(0x22, 0x26, 0x2d);
        QColor border = active_bank ? kAccent : kBorder;

        // Hit glow: decays linearly over kPulseMs.
        std::int64_t const hit = pad_hit_ms_[static_cast<std::size_t>(idx)];
        if (hit != 0) {
            double const age =
                static_cast<double>(now_ms_ - hit) / static_cast<double>(kPulseMs);
            if (age < 1.0) {
                fill = with_alpha(kAccent, 1.0 - age);
                border = kText;
            }
        }

        p.setPen(QPen(border, active_bank ? 1.4 : 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(cell, 4, 4);

        p.setPen(active_bank ? kText : kMuted);
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

    QFont const big{font().family(), std::max(font().pointSize() + 3, 11)};
    QFont const small{font().family(), std::max(font().pointSize() - 1, 7)};

    // Mode strip: segmented control — one rounded container, three
    // segments, active = accent fill.
    double const seg_h = 28.0;
    double const seg_gap = 3.0;
    double const seg_w = (area.width() - 2 * seg_gap) / 3.0;

    QRectF const container{area.left(), area.top() + 6.0, area.width(), seg_h};
    p.setPen(QPen(kBorder, 1.0));
    p.setBrush(QColor(0x17, 0x1a, 0x1f));
    p.drawRoundedRect(container, 7, 7);

    for (int i = 0; i < 3; ++i) {
        QRectF const seg{container.left() + i * (seg_w + seg_gap),
                         container.top(), seg_w, seg_h};
        mode_rects_[i] = seg;
        bool const on = i == mode_idx;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(kAccent);
            p.drawRoundedRect(seg, 5, 5);
        } else {
            // Inactive segment: subtle well + border so the strip reads as
            // three distinct segments.
            p.setPen(QPen(kBorder, 1.0));
            p.setBrush(QColor(0x1f, 0x23, 0x2a));
            p.drawRoundedRect(seg, 5, 5);
        }
        p.setPen(on ? QColor(0x0e, 0x14, 0x1c) : kMuted);
        p.setFont(small);
        p.drawText(seg, Qt::AlignCenter, QString(mode_names[i]));
    }

    // Pad bank indicator + octave display.
    double const info_y = container.bottom() + 8.0;
    bool const bank_b = engine_.mapping().pad_bank_b();
    p.setFont(big);
    p.setPen(bank_b ? kAccent : kGreen);
    p.drawText(QRectF{area.left(), info_y, area.width() * 0.5, 24},
               Qt::AlignLeft | Qt::AlignVCenter,
               QString("PADS %1").arg(bank_b ? QLatin1Char('B') : QLatin1Char('A')));

    int const oct = engine_.octave();
    QString const oct_text = QString("OCT %1%2")
                                 .arg(oct >= 0 ? QLatin1Char('+') : QLatin1Char('-'))
                                 .arg(oct < 0 ? -oct : oct);
    p.setPen(kText);
    p.drawText(QRectF{area.left(), info_y, area.width() * 0.5, 24},
               Qt::AlignRight | Qt::AlignVCenter, oct_text);

    p.setPen(kMuted);
    p.setFont(small);
    p.drawText(QRectF{area.left(), info_y + 26.0, area.width(), 18},
               Qt::AlignLeft | Qt::AlignVCenter,
               QString("track %1 selected").arg(engine_.selected_track() + 1));
}

void ControllerMirror::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();
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
