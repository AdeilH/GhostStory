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
using theme::with_alpha;

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
    // Match the glass language: hairline border, panel fill, accent select.
    velocity_curve_->setStyleSheet(
        "QComboBox { background: #1b1e24; color: #e9edf3;"
        "  border: 1px solid #2c313b; border-radius: 6px; padding: 0 8px; }"
        "QComboBox:hover { border-color: #4f9dff; }"
        "QComboBox QAbstractItemView { background: #1b1e24; color: #e9edf3;"
        "  border: 1px solid #2c313b; selection-background-color: #4f9dff;"
        "  selection-color: #0d0f13; }");
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

    // Typography tiers: tiny tracked labels left, demi-bold values right.
    QFont const label_font = theme::label_font(font());
    QFont const value_font = theme::value_font(font(), 9);
    QFont const tiny{font().family(), std::max(font().pointSize() - 2, 6)};

    double const x0 = 12.0;
    double const meter_x = full.right() - 12.0 - 2.0 * kMeterW - kMeterGap;
    double const text_w = meter_x - x0 - 8.0;

    // One health row: tiny tracked label on the left, colored value pinned
    // to the right edge of the text column.
    auto paint_row = [&](double y, char const* label, QString const& value,
                         QColor const& value_color) {
        p.setPen(kMuted);
        p.setFont(label_font);
        p.drawText(QRectF{x0, y, text_w, 13.0},
                   Qt::AlignLeft | Qt::AlignVCenter, QString(label));
        p.setPen(value_color);
        p.setFont(value_font);
        p.drawText(QRectF{x0, y, text_w, 13.0},
                   Qt::AlignRight | Qt::AlignVCenter, value);
    };

    double y = full.top() + 12.0;

    // --- device LED (with a soft ring while connected) + state ---
    QColor const led = connected ? kGreen : theme::kRecord;
    if (connected) {
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(kGreen, 0.22));
        p.drawEllipse(QPointF{x0 + 5.0, y + 7.0}, 8.5, 8.5);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(led);
    p.drawEllipse(QPointF{x0 + 5.0, y + 7.0}, 5.0, 5.0);
    p.setPen(kMuted);
    p.setFont(label_font);
    p.drawText(QRectF{x0 + 18.0, y, text_w - 18.0, 9.0},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("device"));
    p.setPen(connected ? kText : theme::kRecord);
    p.setFont(value_font);
    p.drawText(QRectF{x0 + 18.0, y + 8.0, text_w - 18.0, 14.0},
               Qt::AlignLeft | Qt::AlignVCenter,
               connected ? QStringLiteral("MPK Mini") : QStringLiteral("no device"));
    y += 28.0;

    // Hairline under the header block.
    p.setPen(QPen(with_alpha(theme::kBorder, 0.8), 1.0));
    p.drawLine(QPointF{x0, y - 4.0}, QPointF{meter_x - 8.0, y - 4.0});
    y += 4.0;

    // --- health rows ---
    paint_row(y, "xruns",
              QString::number(static_cast<qulonglong>(stats.xruns)),
              stats.xruns > 0 ? theme::kRecord : kText);
    y += 16.0;
    paint_row(y, "callback",
              QString("%1 µs").arg(stats.last_callback_us),
              kText);
    y += 16.0;
    paint_row(y, "cb max",
              QString("%1 µs").arg(stats.max_callback_us),
              stats.max_callback_us > 1000 ? theme::kRecord : kMuted);
    y += 16.0;
    paint_row(y, "engine",
              QString::fromStdString(engine_.audio().backend_name()),
              running ? kGreen : theme::kRecord);
    y += 16.0;
    paint_row(y, "rate",
              QString("%1 · %2 f").arg(stats.sample_rate)
                  .arg(stats.buffer_size),
              kMuted);
    y += 20.0;

    p.setPen(kMuted);
    p.setFont(label_font);
    p.drawText(QRectF{x0, y, text_w, 11.0},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("vel curve"));

    // --- master level meter: two thin vertical bars, green→yellow→red ---
    double const meter_top = full.top() + 12.0;
    double const meter_h = velocity_curve_->y() - meter_top - 34.0;
    double const meter_bot = meter_top + meter_h;

    p.setFont(tiny);
    p.setPen(kMuted);
    p.drawText(QRectF{meter_x - 20.0, meter_bot + 4.0,
                      2.0 * kMeterW + kMeterGap + 20.0, 12.0},
               Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("L R"));

    for (int ch = 0; ch < 2; ++ch) {
        double const mx = meter_x + static_cast<double>(ch) * (kMeterW + kMeterGap);
        QRectF const slot{mx, meter_top, kMeterW, meter_h};
        theme::draw_well(p, slot, 3);

        QLinearGradient grad(0.0, meter_bot, 0.0, meter_top);
        grad.setColorAt(0.0, kGreen);
        grad.setColorAt(0.68, QColor(0xd8, 0xc0, 0x3c));
        grad.setColorAt(1.0, theme::kRecord);

        double const fill_h = meter_h * std::clamp(static_cast<double>(level_), 0.0, 1.0);
        if (fill_h > 0.5) {
            // Gradient is defined in painter coordinates over the whole
            // slot, so the color at the current fill top is truthful.
            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawRoundedRect(
                QRectF{slot.left() + 1.5, meter_bot - fill_h, kMeterW - 3.0,
                       fill_h},
                2, 2);
        }
    }
}

}  // namespace addu94::ui
