// ghost_story — src/ui/health_strip.cpp
#include "ui/health_strip.hpp"

#include "ui/theme.hpp"

#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QResizeEvent>
#include <QSettings>

#include <algorithm>

namespace ghost_story::ui {

namespace {

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

    // Velocity curve: the engine's live note-on mapping (MIDI thread reads
    // it per note). The combo order matches the VelocityCurve enum.
    velocity_curve_ = new QComboBox(this);
    velocity_curve_->addItems({QStringLiteral("Linear"), QStringLiteral("Log"),
                               QStringLiteral("Exp"),
                               QStringLiteral("Hard-floor")});
    velocity_curve_->setToolTip(
        "Velocity curve for the instrument path — Log lifts soft strikes "
        "on mini keys (default), Exp rewards hard hits, Hard-floor "
        "removes the quiet bottom end entirely");
    velocity_curve_->setFixedSize(200, 22);
    connect(velocity_curve_, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                engine_.set_velocity_curve(
                    static_cast<music::VelocityCurve>(index));
            });

    // Theme picker: flips the process-global palette (theme::set_theme),
    // persists the choice, and lets the paint cycle do the rest — every
    // custom-painted panel reads pal() on its next 30 Hz frame, and the
    // MainWindow tick re-styles window + app palette when the generation
    // moves. The combos are the only widgets needing stylesheet text.
    theme_ = new QComboBox(this);
    for (int i = 0; i < theme::kThemeCount; ++i) {
        theme_->addItem(QString::fromUtf8(theme::theme_name(i)));
    }
    theme_->setCurrentIndex(theme::theme_index());
    theme_->setFixedSize(200, 22);
    theme_->setToolTip(
        "Theme — the whole instrument repaints instantly and the choice "
        "is remembered across runs");
    connect(theme_, &QComboBox::currentIndexChanged, this, [this](int index) {
        theme::set_theme(index);
        QSettings settings;
        settings.setValue("ui/theme", index);
        style_combos();
    });
    style_combos();

    refresh();
}

void HealthStrip::style_combos() {
    // Match the theme's surface language: panel-tone fill, hairline
    // border, accent hover/selection. Derived, never hardcoded — a
    // theme switch re-runs this (refresh() watches the generation).
    QString const qss = theme::combo_stylesheet();
    velocity_curve_->setStyleSheet(qss);
    theme_->setStyleSheet(qss);
    theme_generation_ = theme::generation();
}

void HealthStrip::refresh() {
    // Theme switched elsewhere (first run before ctor wiring, or a future
    // menu): restyle the stylesheet-driven children.
    if (theme::generation() != theme_generation_) {
        style_combos();
    }
    // Meter: honest peak from the RT tap, peak-hold with decay at the UI
    // tick rate (0.95/frame ≈ falls ~1 dB per 40 ms — transients stay
    // visible between the 30 Hz frames).
    float const fresh = engine_.master_level();
    level_ = std::max(fresh, level_ * 0.95f);
    // Keep the combo honest when the curve changed elsewhere (KIT knob).
    int const idx = static_cast<int>(engine_.velocity_curve());
    if (velocity_curve_->currentIndex() != idx) {
        velocity_curve_->blockSignals(true);
        velocity_curve_->setCurrentIndex(idx);
        velocity_curve_->blockSignals(false);
    }
    update();
}

void HealthStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Bottom stack, top-down: theme caption / theme combo / master fader /
    // velocity caption / velocity combo. paintEvent draws the captions,
    // fader and meter bounds from these same offsets, so the stack stays
    // in sync by construction (the panel is height-pinned by the mirror's
    // 252px floor — every offset here is budgeted for that height).
    velocity_curve_->move(12, height() - 30);
    theme_->move(12, height() - 108);  // above the master fader stack
}

void HealthStrip::set_master_from_pos(QPointF const& pos) {
    double const rel = std::clamp(
        (pos.x() - master_rect_.left()) / std::max(master_rect_.width(), 1.0),
        0.0, 1.0);
    // 0..1 lane → 0..127 knob param (0..1.5 gain; unity ≈ 85).
    engine_.command({mapping::Action::MasterVolume,
                     static_cast<int>(std::lround(rel * 127.0)), 0});
    update();
}

void HealthStrip::mousePressEvent(QMouseEvent* event) {
    if (master_rect_.adjusted(-6.0, -8.0, 6.0, 8.0)
            .contains(event->position())) {
        set_master_from_pos(event->position());
        return;
    }
    QWidget::mousePressEvent(event);
}

void HealthStrip::mouseMoveEvent(QMouseEvent* event) {
    // Drag continues only while the button is held (press starts it).
    if (event->buttons() & Qt::LeftButton &&
        master_rect_.adjusted(-6.0, -8.0, 6.0, 8.0)
            .contains(event->position())) {
        set_master_from_pos(event->position());
        return;
    }
    QWidget::mouseMoveEvent(event);
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
    // to the right edge of the text column. Box height from the font —
    // drawText clips to the rect, and hardcoded heights shaved glyph
    // bottoms on font resolutions whose baseline landed lower.
    auto paint_row = [&](double y, char const* label, QString const& value,
                         QColor const& value_color) {
        double const row_h =
            static_cast<double>(QFontMetrics{label_font}.height());
        p.setPen(theme::pal().muted);
        p.setFont(label_font);
        p.drawText(QRectF{x0, y, text_w, row_h},
                   Qt::AlignLeft | Qt::AlignVCenter, QString(label));
        p.setPen(value_color);
        p.setFont(value_font);
        p.drawText(QRectF{x0, y, text_w, row_h},
                   Qt::AlignRight | Qt::AlignVCenter, value);
    };

    double y = full.top() + 12.0;

    // --- device LED (with a soft ring while connected) + state ---
    QColor const led = connected ? theme::pal().green : theme::pal().record;
    if (connected) {
        p.setPen(Qt::NoPen);
        p.setBrush(with_alpha(theme::pal().green, 0.22));
        p.drawEllipse(QPointF{x0 + 5.0, y + 7.0}, 8.5, 8.5);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(led);
    p.drawEllipse(QPointF{x0 + 5.0, y + 7.0}, 5.0, 5.0);
    p.setPen(theme::pal().muted);
    p.setFont(label_font);
    p.drawText(QRectF{x0 + 18.0, y, text_w - 18.0, 9.0},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("device"));
    p.setPen(connected ? theme::pal().text : theme::pal().record);
    p.setFont(value_font);
    p.drawText(QRectF{x0 + 18.0, y + 8.0, text_w - 18.0, 14.0},
               Qt::AlignLeft | Qt::AlignVCenter,
               connected ? QStringLiteral("MPK Mini") : QStringLiteral("no device"));
    y += 24.0;

    // Hairline under the header block.
    p.setPen(QPen(with_alpha(theme::pal().border, 0.8), 1.0));
    p.drawLine(QPointF{x0, y - 4.0}, QPointF{meter_x - 8.0, y - 4.0});
    y += 4.0;

    // --- health rows (15px pitch: the panel is height-pinned and the
    // bottom stack needs its room — see resizeEvent) ---
    paint_row(y, "xruns",
              QString::number(static_cast<qulonglong>(stats.xruns)),
              stats.xruns > 0 ? theme::pal().record : theme::pal().text);
    y += 15.0;
    paint_row(y, "callback",
              QString("%1 µs").arg(stats.last_callback_us),
              theme::pal().text);
    y += 15.0;
    paint_row(y, "cb max",
              QString("%1 µs").arg(stats.max_callback_us),
              stats.max_callback_us > 1000 ? theme::pal().record : theme::pal().muted);
    y += 15.0;
    paint_row(y, "engine",
              QString::fromStdString(engine_.audio().backend_name()),
              running ? theme::pal().green : theme::pal().record);
    y += 15.0;
    paint_row(y, "rate",
              QString("%1 · %2 f").arg(stats.sample_rate)
                  .arg(stats.buffer_size),
              theme::pal().muted);
    y += 6.0;

    // --- bottom stack captions: the combos only show their current value
    // ("Graphite", "Log"), which read as mystery settings until named.
    // Offsets mirror resizeEvent's combo positions (caption directly
    // above each combo). Boxes are 14px — the font's line height: the old
    // 11px boxes clipped the cap tops off with AlignBottom (drawText
    // clips to the rect), shaving T/M/V into "IHEME"-style fragments.
    p.setFont(label_font);
    p.setPen(theme::pal().muted);
    p.drawText(QRectF{x0, full.bottom() - 48.0, 120.0, 14.0},
               Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("velocity"));
    p.drawText(QRectF{x0, full.bottom() - 127.0, 120.0, 14.0},
               Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("theme"));

    // --- master fader (above the velocity caption): thin lane, drag →
    // MasterVolume command (same one EDIT knob 8 emits). 100% = unity. --
    float const mg = engine_.master_gain();
    double const lane_y = static_cast<double>(height()) - 66.0;
    QRectF const lane{x0, lane_y,
                      static_cast<double>(velocity_curve_->width()), 4.0};
    master_rect_ = lane.adjusted(-6.0, -10.0, 6.0, 10.0);
    // Label left, value right — the same two-column read as every health
    // row above. (The old fixed 60px label box + value at +64 collided
    // once the user's font ran wide: "MASTER" and "100%" printed on top of
    // each other. Right-aligning the value to the lane edge can't collide
    // regardless of font metrics.)
    p.setFont(label_font);
    p.setPen(theme::pal().muted);
    p.drawText(QRectF{x0, lane_y - 19.0, 60.0, 14.0},
               Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("master"));
    p.setPen(theme::pal().text);
    p.setFont(value_font);
    p.drawText(QRectF{x0, lane_y - 19.0,
                      static_cast<double>(velocity_curve_->width()), 14.0},
               Qt::AlignRight | Qt::AlignBottom,
               QString("%1%").arg(std::lround(static_cast<double>(mg) * 100.0)));
    theme::draw_well(p, lane.adjusted(-3.0, -3.0, 3.0, 3.0), 4.0);
    double const mfrac =
        std::clamp(static_cast<double>(mg) / 1.5, 0.0, 1.0);
    double const mfill = lane.width() * mfrac;
    QLinearGradient mgrad{lane.topLeft(), lane.bottomLeft()};
    mgrad.setColorAt(0.0, theme::pal().text);
    mgrad.setColorAt(1.0, theme::pal().green);
    p.setPen(Qt::NoPen);
    p.setBrush(mgrad);
    p.drawRoundedRect(QRectF{lane.left(), lane.top(), mfill, lane.height()},
                      2, 2);
    p.setBrush(theme::pal().text);
    p.drawEllipse(QPointF{lane.left() + mfill, lane.center().y()}, 5.0, 5.0);

    // --- master level meter: two thin vertical bars, green→yellow→red ---
    // Bottom bounded by the theme caption: theme_->y() - 36 = caption top
    // (theme_y - 16) minus the L R caption's 12px + 4px gaps — the old
    // math keyed off the velocity combo and let the bars run down BEHIND
    // the theme combo, occluding the lower third of the meter.
    double const meter_top = full.top() + 12.0;
    double const meter_h = theme_->y() - 36.0 - meter_top;
    double const meter_bot = meter_top + meter_h;

    p.setFont(tiny);
    p.setPen(theme::pal().muted);
    // One caption per bar, centered under it (the old single "L R" label
    // sat between the bars). 14px tall: the 10pt line box is ~15.6px, and
    // drawText clips to the rect — in a 12px box the L's foot landed on
    // the clipped baseline and the pair read as "I R".
    for (int ch = 0; ch < 2; ++ch) {
        double const mx =
            meter_x + static_cast<double>(ch) * (kMeterW + kMeterGap);
        p.drawText(QRectF{mx - 6.0, meter_bot + 4.0, kMeterW + 12.0, 14.0},
                   Qt::AlignHCenter | Qt::AlignTop,
                   ch == 0 ? QStringLiteral("L") : QStringLiteral("R"));
    }

    for (int ch = 0; ch < 2; ++ch) {
        double const mx = meter_x + static_cast<double>(ch) * (kMeterW + kMeterGap);
        QRectF const slot{mx, meter_top, kMeterW, meter_h};
        theme::draw_well(p, slot, 3);

        QLinearGradient grad(0.0, meter_bot, 0.0, meter_top);
        grad.setColorAt(0.0, theme::pal().green);
        grad.setColorAt(0.68, QColor(0xd8, 0xc0, 0x3c));
        grad.setColorAt(1.0, theme::pal().record);

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

}  // namespace ghost_story::ui
