// addu94 — src/ui/health_strip.cpp
#include "ui/health_strip.hpp"

#include "ui/theme.hpp"

#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QResizeEvent>

#include <algorithm>

namespace addu94::ui {

namespace {

using theme::kBorder;
using theme::kGreen;
using theme::kMuted;
using theme::kText;

// Level meter geometry (relative to the strip's right edge).
constexpr double kMeterW = 9.0;
constexpr double kMeterGap = 3.0;

}  // namespace

HealthStrip::HealthStrip(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setFixedWidth(224);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setToolTip("Audio + device health — connection, xruns, callback time, "
               "live master level");

    // Velocity curve (folded in from the old Device tab; stored in-widget
    // until M6 wires the synth-side curve).
    velocity_curve_ = new QComboBox(this);
    velocity_curve_->addItems({QStringLiteral("Linear"), QStringLiteral("Log"),
                               QStringLiteral("Exp"),
                               QStringLiteral("Hard-floor")});
    velocity_curve_->setToolTip(
        "Velocity curve for the instrument path (stored in the UI until M6 "
        "wires the synth-side curve)");
    velocity_curve_->setFixedSize(200, 22);
    connect(velocity_curve_, &QComboBox::currentIndexChanged, this,
            [](int index) {
                // View state for now; logged via the tooltip contract until
                // the synth-side curve lands (M6).
                (void)index;
            });

    refresh();
}

void HealthStrip::refresh() {
    // Meter: honest peak from the RT tap, peak-hold with decay at the UI
    // tick rate (0.95/frame ≈ falls ~1 dB per 40 ms — transients stay
    // visible between the 30 Hz frames).
    float const fresh = engine_.master_level();
    level_ = std::max(fresh, level_ * 0.95f);
    update();
}

void HealthStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    velocity_curve_->move(12, height() - 30);
}

void HealthStrip::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF const full = QRectF(rect());
    theme::draw_panel(p, full);

    auto const stats = engine_.audio().stats();
    bool const connected = engine_.device_connected();
    bool const running = engine_.audio_running();

    double const x0 = 12.0;
    double const meter_x = full.right() - 12.0 - 2.0 * kMeterW - kMeterGap;
    double const text_w = meter_x - x0 - 8.0;

    QFont const small{font().family(), std::max(font().pointSize() - 1, 7)};
    QFont const tiny{font().family(), std::max(font().pointSize() - 2, 6)};
    double y = full.top() + 12.0;

    // --- device LED + label ---
    p.setPen(Qt::NoPen);
    p.setBrush(connected ? kGreen : theme::kRecord);
    p.drawEllipse(QPointF{x0 + 5.0, y + 7.0}, 5.0, 5.0);
    p.setPen(kText);
    p.setFont(small);
    p.drawText(QRectF{x0 + 16.0, y, text_w - 16.0, 16.0},
               Qt::AlignLeft | Qt::AlignVCenter,
               connected ? QStringLiteral("MPK Mini") : QStringLiteral("no device"));
    y += 20.0;

    // --- xruns (red when > 0) ---
    p.setFont(small);
    p.setPen(stats.xruns > 0 ? theme::kRecord : kMuted);
    p.drawText(QRectF{x0, y, text_w, 15.0}, Qt::AlignLeft | Qt::AlignVCenter,
               QString("xruns %1").arg(static_cast<qulonglong>(stats.xruns)));
    y += 17.0;

    // --- callback time ---
    p.setPen(kMuted);
    p.drawText(QRectF{x0, y, text_w, 15.0}, Qt::AlignLeft | Qt::AlignVCenter,
               QString("cb %1 µs / max %2").arg(stats.last_callback_us)
                   .arg(stats.max_callback_us));
    y += 17.0;

    // --- backend + running state ---
    p.setPen(running ? kGreen : theme::kRecord);
    p.drawText(QRectF{x0, y, text_w, 15.0}, Qt::AlignLeft | Qt::AlignVCenter,
               QString("%1 %2")
                   .arg(QString::fromStdString(engine_.audio().backend_name()))
                   .arg(running ? QStringLiteral("running")
                                : QStringLiteral("STOPPED")));
    y += 17.0;

    p.setFont(tiny);
    p.setPen(kMuted);
    p.drawText(QRectF{x0, y, text_w, 13.0}, Qt::AlignLeft | Qt::AlignVCenter,
               QString("%1 Hz · %2 f").arg(stats.sample_rate)
                   .arg(stats.buffer_size));
    y += 17.0;

    p.setFont(tiny);
    p.drawText(QRectF{x0, y, text_w, 13.0}, Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("velocity curve"));
    y += 15.0;

    // --- master level meter: two thin vertical bars, green→yellow→red ---
    double const meter_top = full.top() + 12.0;
    double const meter_h = velocity_curve_->y() - meter_top - 30.0;
    double const meter_bot = meter_top + meter_h;

    p.setFont(tiny);
    p.setPen(kMuted);
    p.drawText(QRectF{meter_x - 20.0, meter_bot + 4.0,
                      2.0 * kMeterW + kMeterGap + 20.0, 12.0},
               Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("L R"));

    for (int ch = 0; ch < 2; ++ch) {
        double const mx = meter_x + static_cast<double>(ch) * (kMeterW + kMeterGap);
        QRectF const slot{mx, meter_top, kMeterW, meter_h};
        p.setPen(QPen(kBorder, 1.0));
        p.setBrush(QColor(0x17, 0x1a, 0x1f));
        p.drawRoundedRect(slot, 3, 3);

        QLinearGradient grad(0.0, meter_bot, 0.0, meter_top);
        grad.setColorAt(0.0, kGreen);
        grad.setColorAt(0.68, QColor(0xd8, 0xc0, 0x3c));
        grad.setColorAt(1.0, theme::kRecord);

        double const fill_h = meter_h * std::clamp(static_cast<double>(level_), 0.0, 1.0);
        if (fill_h > 0.5) {
            // Gradient is defined in painter coordinates over the whole
            // slot, so the color at the current fill top is truthful.
            p.setBrush(grad);
            p.drawRoundedRect(
                QRectF{slot.left(), meter_bot - fill_h, kMeterW, fill_h}, 3, 3);
        }
    }
}

}  // namespace addu94::ui
