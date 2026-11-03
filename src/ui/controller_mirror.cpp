// addu94 — src/ui/controller_mirror.cpp
#include "ui/controller_mirror.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <chrono>

namespace addu94::ui {

namespace {

// Dark theme (spec §9). Keep in sync with the QApplication palette in
// app_gui.cpp.
constexpr QColor kBg{0x1a, 0x1d, 0x21};
constexpr QColor kPanel{0x23, 0x27, 0x2c};
constexpr QColor kBlue{0x4f, 0x9c, 0xf9};
constexpr QColor kGreen{0x37, 0xb2, 0x6c};
[[maybe_unused]] constexpr QColor kRed{0xd9, 0x53, 0x4f};  // record state lands with M5
constexpr QColor kText{0xcf, 0xd6, 0xdd};
constexpr QColor kDim{0x6b, 0x74, 0x7e};

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

QColor with_alpha(QColor const& c, double a) noexcept {
    QColor out = c;
    out.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return out;
}

}  // namespace

ControllerMirror::ControllerMirror(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setMinimumHeight(230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
    p.fillRect(full, kBg);

    // Top strip: 8 knobs. Bottom: pads on the left, status on the right.
    double const pad = 6.0;
    QRectF const knobs{pad, pad, full.width() - 2 * pad, 104.0};
    double const bottom_y = knobs.bottom() + pad;
    double const pad_w = 4.0 * 46.0 + 10.0;
    QRectF const pads{pad, bottom_y, pad_w, full.height() - bottom_y - pad};
    QRectF const status{pads.right() + pad, bottom_y,
                        full.right() - pad - pads.right(), pads.height()};

    p.setPen(Qt::NoPen);
    p.setBrush(kPanel);
    p.drawRoundedRect(knobs, 6, 6);
    p.drawRoundedRect(pads, 6, 6);
    p.drawRoundedRect(status, 6, 6);

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
    double const cy = area.top() + area.height() * 0.36;
    double const r = std::min(cell_w, area.height()) * 0.36;

    for (int i = 0; i < 8; ++i) {
        double const cx = area.left() + cell_w * (i + 0.5);
        QRectF const arc_box{cx - r, cy - r, 2 * r, 2 * r};

        // Base arc: 270° sweep starting at 225° (Qt angles: 1/16 deg,
        // counter-clockwise positive).
        QPen base_pen(kDim, 3.0, Qt::SolidLine, Qt::RoundCap);
        p.setPen(base_pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(arc_box, 225 * 16, -kKnobSweep * 16);

        // Value arc.
        int const v = engine_.mapping().knob_value(i);
        double const frac = static_cast<double>(v) / 127.0;
        if (frac > 0.0) {
            p.setPen(QPen(kBlue, 3.0, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_box, 225 * 16,
                      -static_cast<int>(kKnobSweep * frac) * 16);
        }

        p.setPen(kText);
        p.setFont(small);
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 2, cell_w, 14},
                   Qt::AlignHCenter | Qt::AlignTop, QString::number(v));
        p.setPen(kDim);
        p.setFont(tiny);
        p.drawText(QRectF{cx - cell_w / 2, cy + r + 16, cell_w, 12},
                   Qt::AlignHCenter | Qt::AlignTop, QString(labels[static_cast<std::size_t>(i)]));
    }
}

void ControllerMirror::paint_pads(QPainter& p, QRectF const& area) {
    bool const bank_b = engine_.mapping().pad_bank_b();
    QFont const pad_font{font().family(), std::max(font().pointSize() - 2, 6)};

    double const gap = 4.0;
    double const cell_w = (area.width() - gap) / 4.0;
    double const cell_h = (area.height() - gap) / 4.0;

    for (int idx = 0; idx < kPadCount; ++idx) {
        // Columns 0-1: bank A pads A1..A8 (row-major); columns 2-3: B1..B8.
        int const local = idx % 8;
        int const col_base = idx < 8 ? 0 : 2;
        int const row = local / 2;
        int const col = col_base + local % 2;

        QRectF const cell{area.left() + col * (cell_w + gap),
                          area.top() + row * (cell_h + gap), cell_w, cell_h};

        bool const active_bank = (idx < 8) != bank_b;
        QColor fill = kPanel;
        QColor border = active_bank ? (bank_b ? kBlue : kGreen) : kDim;

        // Hit glow: decays linearly over kPulseMs.
        std::int64_t const hit = pad_hit_ms_[static_cast<std::size_t>(idx)];
        if (hit != 0) {
            double const age =
                static_cast<double>(now_ms_ - hit) / static_cast<double>(kPulseMs);
            if (age < 1.0) {
                fill = with_alpha(kBlue, 1.0 - age);
                border = kText;
            }
        }

        p.setPen(QPen(border, 1.2));
        p.setBrush(fill);
        p.drawRoundedRect(cell, 4, 4);

        p.setPen(active_bank ? kText : kDim);
        p.setFont(pad_font);
        QString const label =
            QString("%1%2").arg(idx < 8 ? 'A' : 'B').arg(local + 1);
        p.drawText(cell, Qt::AlignCenter, label);
    }
}

void ControllerMirror::paint_status(QPainter& p, QRectF const& area) {
    auto const mode = engine_.mapping().mode();
    char const* mode_names[3] = {"PLAY", "EDIT", "STEP"};
    int const mode_idx = mode == mapping::Mode::Edit
                             ? 1
                             : (mode == mapping::Mode::Step ? 2 : 0);

    QFont const big{font().family(), std::max(font().pointSize() + 4, 11)};
    QFont const small{font().family(), std::max(font().pointSize() - 1, 7)};

    // Mode button row: three equal buttons across the top of the strip.
    double const btn_h = 30.0;
    double const btn_w = (area.width() - 2 * 4.0) / 3.0;
    for (int i = 0; i < 3; ++i) {
        QRectF const btn{area.left() + i * (btn_w + 4.0), area.top() + 6.0,
                         btn_w, btn_h};
        mode_rects_[i] = btn;
        bool const on = i == mode_idx;
        p.setPen(QPen(on ? kBlue : kDim, 1.2));
        p.setBrush(on ? with_alpha(kBlue, 0.25) : kPanel);
        p.drawRoundedRect(btn, 4, 4);
        p.setPen(on ? kText : kDim);
        p.setFont(small);
        p.drawText(btn, Qt::AlignCenter, QString(mode_names[i]));
    }

    // Pad bank indicator + octave display.
    double const info_y = area.top() + btn_h + 14.0;
    bool const bank_b = engine_.mapping().pad_bank_b();
    p.setFont(big);
    p.setPen(bank_b ? kBlue : kGreen);
    p.drawText(QRectF{area.left(), info_y, area.width(), 26}, Qt::AlignLeft,
               QString("PADS %1").arg(bank_b ? "B" : "A"));

    int const oct = engine_.octave();
    QString const oct_text = QString("OCT %1%2")
                                 .arg(oct >= 0 ? "+" : "-")
                                 .arg(oct < 0 ? -oct : oct);
    p.setPen(kText);
    p.drawText(QRectF{area.left(), info_y, area.width(), 26}, Qt::AlignRight,
               oct_text);

    p.setPen(kDim);
    p.setFont(small);
    p.drawText(QRectF{area.left(), info_y + 30.0, area.width(), 18},
               Qt::AlignLeft, QString("mode %1 | track %2")
                                  .arg(mode_names[mode_idx])
                                  .arg(engine_.selected_track() + 1));
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
