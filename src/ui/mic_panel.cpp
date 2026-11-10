// ghost_story — src/ui/mic_panel.cpp
#include "ui/mic_panel.hpp"

#include "ui/theme.hpp"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace ghost_story::ui {

namespace {

using theme::with_alpha;

constexpr int kDurCount = 3;
constexpr int kDurSeconds[kDurCount] = {1, 2, 4};

// Inset toggle chip (shared with the transport/track-strip language).
void paint_toggle_chip(QPainter& p, QRectF const& r, QString const& glyph,
                       bool on, QFont const& font) {
    QLinearGradient fill{r.topLeft(), r.bottomLeft()};
    if (on) {
        fill.setColorAt(0.0, with_alpha(theme::pal().accent, 0.34));
        fill.setColorAt(1.0, with_alpha(theme::pal().accent, 0.16));
        p.setPen(QPen(theme::pal().accent, 1.0));
    } else {
        fill.setColorAt(0.0, theme::shift(theme::pal().panel_top, 4));
        fill.setColorAt(1.0, theme::shift(theme::pal().panel_top, -6));
        p.setPen(QPen(theme::pal().border, 1.0));
    }
    p.setBrush(fill);
    p.drawRoundedRect(r, 6, 6);
    p.setPen(on ? theme::pal().text : with_alpha(theme::pal().muted, 0.8));
    p.setFont(font);
    p.drawText(r, Qt::AlignCenter, glyph);
}

}  // namespace

MicPanel::MicPanel(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip(
        "MIC — the live input channel.\n"
        "Status dot — green while the audio backend is actually "
        "delivering mic frames; dim when no capture device opened.\n"
        "MONITOR — mix the mic into the master output live (so you can "
        "sing/play over the beat). Default OFF: an open mic through "
        "speakers feeds back — turn it on with HEADPHONES, or keep the "
        "speakers quiet and the gain low.\n"
        "gain — input gain 0..200%, applied before monitoring and before "
        "a grab.\n"
        "GRAB — take the last 1/2/4 seconds of captured mic audio and "
        "load it as a DRUM PAD sample (bank A): pick the duration, pick "
        "the target pad 1..8, hit GRAB, then the pad plays your voice. "
        "Grabs are peak-normalized so they land at a usable level.");
    setMinimumWidth(212);
    setMaximumWidth(248);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void MicPanel::refresh() { update(); }

void MicPanel::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF const full = QRectF(rect());
    theme::draw_panel(p, full);

    QFont const label = theme::label_font(font());
    QFont const chip_font = theme::value_font(font(), 9);
    QFont const tiny{font().family(), 8};
    QFont const micro = theme::label_font(font());
    QFontMetrics const fm{chip_font};

    double const pad = 8.0;
    QRectF const inner = full.adjusted(pad, pad, -pad, -pad);
    double const x0 = inner.left();
    double const w = inner.width();

    // --- header: mic label + status dot ---
    double y = inner.top();
    p.setPen(theme::pal().muted);
    p.setFont(label);
    p.drawText(QRectF{x0, y, w, 14.0}, Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("mic"));
    bool const active = engine_.mic_active();
    QPointF const dot_c{x0 + w - 7.0, y + 7.0};
    if (active) {
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme::pal().green, 0.22));
        p.drawEllipse(dot_c, 8.0, 8.0);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(active ? theme::pal().green : with_alpha(theme::pal().record, 0.55));
    p.drawEllipse(dot_c, 4.0, 4.0);
    y += 18.0;

    // --- monitor chip (full width) ---
    QRectF const mon{x0, y, w, 20.0};
    monitor_rect_ = mon;
    paint_toggle_chip(p, mon,
                      engine_.mic_monitor() ? QStringLiteral("MONITOR ON")
                                            : QStringLiteral("monitor off"),
                      engine_.mic_monitor(), chip_font);
    y += 24.0;

    // --- gain lane (0..200%) ---
    // Label boxes sized from the font's own metrics: drawText clips to the
    // rect, and hardcoded 10px boxes are shorter than the 9pt line box —
    // on font resolutions whose baseline lands lower, the glyph bottoms
    // slice off.
    double const micro_h =
        static_cast<double>(QFontMetrics{micro}.height());
    double const chip_h = static_cast<double>(fm.height());
    p.setPen(theme::pal().muted);
    p.setFont(micro);
    p.drawText(QRectF{x0, y, 60.0, micro_h}, Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("gain"));
    float const gain = engine_.mic_gain();
    p.setPen(theme::pal().text);
    p.setFont(chip_font);
    p.drawText(QRectF{x0 + w - 56.0, y, 56.0, chip_h},
               Qt::AlignRight | Qt::AlignVCenter,
               QString("%1%").arg(std::lround(static_cast<double>(gain) * 100.0)));
    y += 12.0;
    QRectF const lane{x0, y, w, 4.0};
    gain_rect_ = lane.adjusted(-4.0, -8.0, 4.0, 8.0);
    theme::draw_well(p, lane.adjusted(-3.0, -3.0, 3.0, 3.0), 4.0);
    double const gfrac = std::clamp(static_cast<double>(gain) / 2.0, 0.0, 1.0);
    double const gfill = lane.width() * gfrac;
    QLinearGradient ggrad{lane.topLeft(), lane.bottomLeft()};
    ggrad.setColorAt(0.0, theme::pal().accent);
    ggrad.setColorAt(1.0, theme::pal().green);
    p.setPen(Qt::NoPen);
    p.setBrush(ggrad);
    p.drawRoundedRect(QRectF{lane.left(), lane.top(), gfill, lane.height()},
                      2, 2);
    p.setBrush(theme::pal().text);
    p.drawEllipse(QPointF{lane.left() + gfill, lane.center().y()}, 5.0, 5.0);
    y += 12.0;

    // --- level meter (post-gain peak, 30 fps decay) ---
    float const fresh = engine_.mic_level();
    level_ = std::max(fresh, level_ * 0.90f);
    QRectF const meter{x0, y, w, 6.0};
    theme::draw_well(p, meter.adjusted(-3.0, -3.0, 3.0, 3.0), 4.0);
    double const lfill =
        meter.width() * std::clamp(static_cast<double>(level_), 0.0, 1.0);
    QLinearGradient lgrad{meter.topLeft(), meter.topRight()};
    lgrad.setColorAt(0.0, theme::pal().green);
    lgrad.setColorAt(0.75, QColor(0xd8, 0xc0, 0x3c));
    lgrad.setColorAt(1.0, theme::pal().record);
    if (lfill > 0.5) {
        p.setPen(Qt::NoPen);
        p.setBrush(lgrad);
        p.drawRoundedRect(QRectF{meter.left(), meter.top(), lfill,
                                 meter.height()},
                          2, 2);
    }
    p.setPen(theme::pal().muted);
    p.setFont(micro);
    // Status line a clear step below the meter well (the old +2 top put
    // the caps inside the well's bottom edge, which read as sliced text).
    double const status_y = meter.bottom() + 6.0;
    double const status_h =
        static_cast<double>(QFontMetrics{micro}.height());
    p.drawText(QRectF{x0, status_y, w, status_h},
               Qt::AlignLeft | Qt::AlignTop,
               active ? QStringLiteral("live input")
                      : QStringLiteral("no input"));
    y = status_y + status_h + 6.0;

    // --- grab section ---
    p.setPen(theme::pal().muted);
    p.setFont(micro);
    p.drawText(QRectF{x0, y, w, micro_h}, Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("grab to pad"));
    y += 12.0;

    // Duration chips: 1s / 2s / 4s (thirds of the row).
    double const dw = (w - 2.0 * 4.0) / 3.0;
    for (int i = 0; i < kDurCount; ++i) {
        QRectF const chip{x0 + i * (dw + 4.0), y, dw, 18.0};
        dur_rects_[static_cast<std::size_t>(i)] = chip;
        bool const on = kDurSeconds[i] == grab_seconds_;
        paint_toggle_chip(p, chip,
                          QString("%1s").arg(kDurSeconds[i]), on, tiny);
    }
    y += 22.0;

    // Target pad chips: bank A pads P1..P8 (notes 36..43) as a 2x4 grid —
    // one row of 8 made each chip ~22px wide (smaller than a fingertip);
    // two rows of 4 give real hit targets for a few pixels more.
    double const pw = (w - 3.0 * 3.0) / 4.0;
    for (int i = 0; i < 8; ++i) {
        int const row = i / 4;
        int const col = i % 4;
        QRectF const chip{x0 + col * (pw + 3.0), y + row * 21.0, pw, 18.0};
        pad_rects_[static_cast<std::size_t>(i)] = chip;
        bool const on = i == grab_pad_;
        paint_toggle_chip(p, chip, QString("P%1").arg(i + 1), on, tiny);
    }
    y += 22.0 + 18.0 + 4.0;  // two chip rows + gap

    // GRAB button (record-red; pulses softly while capture is live).
    QRectF const btn{x0, y, w, 26.0};
    grab_rect_ = btn;
    QLinearGradient bfill{btn.topLeft(), btn.bottomLeft()};
    bfill.setColorAt(0.0, with_alpha(theme::pal().record, 0.38));
    bfill.setColorAt(1.0, with_alpha(theme::pal().record, 0.18));
    p.setPen(QPen(theme::pal().record, 1.2));
    p.setBrush(bfill);
    p.drawRoundedRect(btn, 8, 8);
    p.setPen(theme::pal().text);
    p.setFont(chip_font);
    p.drawText(btn, Qt::AlignCenter,
               QString("GRAB  \u2192  P%1  \u00b7  %2s")
                   .arg(grab_pad_ + 1)
                   .arg(grab_seconds_));
    y += 30.0;

    // Result / error line after a grab. The standing explainer is a short
    // one-liner now — the full "how it works" lives in the widget tooltip
    // (the old 3–4 line word wrap was most of the panel's visual noise).
    p.setFont(micro);
    if (!grab_status_.isEmpty()) {
        p.setPen(grab_error_ ? theme::pal().record : theme::pal().green);
        p.drawText(QRectF{x0, y, w, inner.bottom() - y},
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   grab_status_);
    } else {
        p.setPen(with_alpha(theme::pal().muted, 0.8));
        p.drawText(QRectF{x0, y, w, 14.0},
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine,
                   QString("last %1s \u2192 pad P%2 on grab")
                       .arg(grab_seconds_)
                       .arg(grab_pad_ + 1));
    }
}

void MicPanel::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    if (monitor_rect_.contains(pos)) {
        engine_.set_mic_monitor(!engine_.mic_monitor());
        update();
        return;
    }
    if (gain_rect_.contains(pos)) {
        dragging_gain_ = true;
        double const rel = std::clamp(
            (pos.x() - (gain_rect_.left() + 4.0)) /
                std::max(gain_rect_.width() - 8.0, 1.0),
            0.0, 1.0);
        engine_.set_mic_gain(static_cast<float>(rel * 2.0));
        update();
        return;
    }
    for (int i = 0; i < kDurCount; ++i) {
        if (dur_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            grab_seconds_ = kDurSeconds[i];
            update();
            return;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (pad_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            grab_pad_ = i;
            update();
            return;
        }
    }
    if (grab_rect_.contains(pos)) {
        std::string err;
        int const pad = grab_pad_;
        double const secs = static_cast<double>(grab_seconds_);
        if (engine_.grab_mic_to_pad(pad, secs, err)) {
            grab_error_ = false;
            grab_status_ = QString("grabbed %1s \u2192 pad P%2 \u2014 hit "
                                   "it in PLAY")
                               .arg(secs)
                               .arg(pad + 1);
            if (grab_hook_) grab_hook_(pad);
        } else {
            grab_error_ = true;
            grab_status_ = QString::fromStdString(err);
        }
        update();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MicPanel::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_gain_ && (event->buttons() & Qt::LeftButton)) {
        QPointF const pos = event->position();
        double const rel = std::clamp(
            (pos.x() - (gain_rect_.left() + 4.0)) /
                std::max(gain_rect_.width() - 8.0, 1.0),
            0.0, 1.0);
        engine_.set_mic_gain(static_cast<float>(rel * 2.0));
        update();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void MicPanel::mouseReleaseEvent(QMouseEvent* event) {
    dragging_gain_ = false;
    QWidget::mouseReleaseEvent(event);
}

}  // namespace ghost_story::ui
