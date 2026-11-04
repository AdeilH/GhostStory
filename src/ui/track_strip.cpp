// addu94 — src/ui/track_strip.cpp
#include "ui/track_strip.hpp"

#include "ui/theme.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace addu94::ui {

namespace {

using theme::kAccent;
using theme::kBorder;
using theme::kMuted;
using theme::kRecord;
using theme::kText;
using theme::with_alpha;

constexpr int kTrackCount = 4;

QString pan_label(int pan) {
    if (pan == 0) return QStringLiteral("C");
    return QString("%1%2").arg(pan < 0 ? QLatin1Char('L') : QLatin1Char('R'))
        .arg(std::abs(pan));
}

}  // namespace

TrackStrips::TrackStrips(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip(
        "Tracks — click a row to select it (MPK pads/keys follow). "
        "M = mute, the dot = record-arm; drag the thin bar for volume "
        "(0..150%), drag the pan lane to set stereo position");
    setMinimumWidth(232);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void TrackStrips::set_track_hook(TrackHook hook) { hook_ = std::move(hook); }

void TrackStrips::refresh() { update(); }

TrackStrips::RowGeom TrackStrips::row_geom(int track) const noexcept {
    double const w = static_cast<double>(width());
    double const h = static_cast<double>(height());
    double const pad = 8.0;
    double const row_h = (h - 2.0 * pad) / static_cast<double>(kTrackCount);
    double const y = pad + static_cast<double>(track) * row_h;

    RowGeom g;
    g.full = {pad + 2.0, y + 3.0, w - 2.0 * (pad + 2.0), row_h - 6.0};
    g.chip = QRectF{g.full.left() + 2, g.full.top() + 5, 26, 22};

    double const right = g.full.right();
    g.mute = QRectF{right - 60.0, g.full.top() + 4.0, 28.0, 24.0};
    g.arm = QRectF{right - 28.0, g.full.top() + 4.0, 28.0, 24.0};

    double const bar_y = g.full.top() + 40.0;
    g.vol = QRectF{g.full.left() + 2.0, bar_y,
                   g.full.width() - 4.0 - 44.0, 16.0};
    g.pan = QRectF{g.full.right() - 40.0, bar_y, 40.0, 16.0};
    return g;
}

void TrackStrips::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    theme::draw_panel(p, QRectF(rect()));

    QFont const name_font{font().family(), 10, QFont::DemiBold};
    QFont const tiny_font{font().family(), 8};

    int const selected = engine_.selected_track();
    for (int t = 0; t < kTrackCount; ++t) {
        RowGeom const g = row_geom(t);
        QColor const hue = theme::track_color(t);

        // Row highlight for the selected track.
        if (t == selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(kAccent, 0.10));
            p.drawRoundedRect(g.full, 8, 8);
            p.setPen(QPen(with_alpha(kAccent, 0.55), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(g.full, 8, 8);
        }

        // Hue chip + name.
        p.setPen(Qt::NoPen);
        p.setBrush(hue);
        p.drawRoundedRect(g.chip, 5, 5);
        p.setPen(QColor(0x10, 0x14, 0x1a));
        p.setFont(name_font);
        p.drawText(g.chip, Qt::AlignCenter, QString("T%1").arg(t + 1));
        p.setPen(t == selected ? kText : kMuted);
        p.drawText(QRectF{g.chip.right() + 8.0, g.chip.top(), 70.0,
                          g.chip.height()},
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("Track %1").arg(t + 1));

        // Mute toggle (M).
        bool const m = mute_[static_cast<std::size_t>(t)];
        p.setPen(QPen(m ? kMuted : kBorder, 1.0));
        p.setBrush(m ? with_alpha(kMuted, 0.35) : QColor(0x17, 0x1a, 0x1f));
        p.drawRoundedRect(g.mute, 5, 5);
        p.setPen(m ? kText : kMuted);
        p.setFont(name_font);
        p.drawText(g.mute, Qt::AlignCenter, QStringLiteral("M"));

        // Arm toggle (record dot).
        bool const a = arm_[static_cast<std::size_t>(t)];
        p.setPen(QPen(a ? kRecord : kBorder, 1.0));
        p.setBrush(a ? with_alpha(kRecord, 0.30) : QColor(0x17, 0x1a, 0x1f));
        p.drawRoundedRect(g.arm, 5, 5);
        p.setBrush(a ? kRecord : kMuted);
        p.setPen(Qt::NoPen);
        double const dot = 8.0;
        p.drawEllipse(QPointF{g.arm.center().x(), g.arm.center().y()},
                      dot / 2.0, dot / 2.0);

        // Volume: 4px bar + draggable handle, 0..150%.
        QRectF const bar{g.vol.left(), g.vol.top() + 6.0, g.vol.width(), 4.0};
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x27, 0x2c, 0x35));
        p.drawRoundedRect(bar, 2, 2);
        double const frac = static_cast<double>(
                                    volume_[static_cast<std::size_t>(t)]) / 150.0;
        double const fill_w = bar.width() * std::clamp(frac, 0.0, 1.0);
        QColor const hue_soft = theme::track_color(t);
        p.setBrush(t == selected ? hue_soft : with_alpha(hue_soft, 0.6));
        p.drawRoundedRect(QRectF{bar.left(), bar.top(), fill_w, bar.height()},
                          2, 2);
        double const hx = bar.left() + fill_w;
        p.setBrush(kText);
        p.drawEllipse(QPointF{hx, bar.center().y()}, 5.0, 5.0);
        p.setPen(kMuted);
        p.setFont(tiny_font);
        p.drawText(QRectF{bar.left(), bar.bottom() + 1.0, bar.width(), 11.0},
                   Qt::AlignLeft | Qt::AlignTop,
                   QString("vol %1%").arg(volume_[static_cast<std::size_t>(t)]));

        // Pan: mini lane with center tick + position dot + label.
        QRectF const lane{g.pan.left(), g.pan.top() + 6.0, g.pan.width(), 4.0};
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x27, 0x2c, 0x35));
        p.drawRoundedRect(lane, 2, 2);
        double const mid = lane.center().x();
        p.setBrush(kBorder);
        p.drawRoundedRect(QRectF{mid - 0.75, lane.top() - 3.0, 1.5,
                                 lane.height() + 6.0},
                          1, 1);
        double const pfrac =
            static_cast<double>(pan_[static_cast<std::size_t>(t)]) / 100.0;
        double const px = mid + pfrac * (lane.width() / 2.0 - 3.0);
        p.setBrush(kText);
        p.drawEllipse(QPointF{px, lane.center().y()}, 4.0, 4.0);
        p.setPen(kMuted);
        p.drawText(QRectF{lane.left(), lane.bottom() + 1.0, lane.width(), 11.0},
                   Qt::AlignHCenter | Qt::AlignTop,
                   pan_label(pan_[static_cast<std::size_t>(t)]));
    }
}

void TrackStrips::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();
    for (int t = 0; t < kTrackCount; ++t) {
        RowGeom const g = row_geom(t);
        if (!g.full.adjusted(-4.0, -4.0, 4.0, 4.0).contains(pos)) continue;

        if (g.mute.contains(pos)) {
            auto& m = mute_[static_cast<std::size_t>(t)];
            m = !m;
            if (hook_) hook_(t, "mute", m ? 1.0 : 0.0);
            update();
            return;
        }
        if (g.arm.contains(pos)) {
            auto& a = arm_[static_cast<std::size_t>(t)];
            a = !a;
            if (hook_) hook_(t, "arm", a ? 1.0 : 0.0);
            update();
            return;
        }
        if (g.vol.adjusted(-6.0, -8.0, 6.0, 8.0).contains(pos)) {
            drag_track_ = t;
            drag_volume_ = true;
            set_volume(t, (pos.x() - g.vol.left()) / g.vol.width() * 150.0,
                       true);
            return;
        }
        if (g.pan.adjusted(-6.0, -8.0, 6.0, 8.0).contains(pos)) {
            drag_track_ = t;
            drag_volume_ = false;
            double const rel = (pos.x() - g.pan.left()) / g.pan.width();
            set_pan(t, std::clamp(rel, 0.0, 1.0) * 200.0 - 100.0, true);
            return;
        }

        // Row click → select the track (same path as the hardware button).
        engine_.command({mapping::Action::TrackSelect, t, 0});
        update();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TrackStrips::mouseMoveEvent(QMouseEvent* event) {
    if (drag_track_ < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    QPointF const pos = event->position();
    RowGeom const g = row_geom(drag_track_);
    if (drag_volume_) {
        double const rel = (pos.x() - g.vol.left()) / g.vol.width();
        set_volume(drag_track_, std::clamp(rel, 0.0, 1.0) * 150.0, true);
    } else {
        double const rel = (pos.x() - g.pan.left()) / g.pan.width();
        set_pan(drag_track_, std::clamp(rel, 0.0, 1.0) * 200.0 - 100.0, true);
    }
}

void TrackStrips::mouseReleaseEvent(QMouseEvent* event) {
    drag_track_ = -1;
    QWidget::mouseReleaseEvent(event);
}

void TrackStrips::mouseDoubleClickEvent(QMouseEvent* event) {
    // Double-click resets volume/pan of the row under the pointer.
    QPointF const pos = event->position();
    for (int t = 0; t < kTrackCount; ++t) {
        RowGeom const g = row_geom(t);
        if (g.vol.adjusted(-6.0, -8.0, 6.0, 8.0).contains(pos)) {
            set_volume(t, 100.0, true);
            return;
        }
        if (g.pan.adjusted(-6.0, -8.0, 6.0, 8.0).contains(pos)) {
            set_pan(t, 0.0, true);
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TrackStrips::set_volume(int track, double pct, bool report) {
    int const v = std::clamp(static_cast<int>(std::lround(pct)), 0, 150);
    volume_[static_cast<std::size_t>(track)] = v;
    if (report && hook_) hook_(track, "volume", static_cast<double>(v));
    update();
}

void TrackStrips::set_pan(int track, double pan, bool report) {
    int const v = std::clamp(static_cast<int>(std::lround(pan)), -100, 100);
    pan_[static_cast<std::size_t>(track)] = v;
    if (report && hook_) hook_(track, "pan", static_cast<double>(v));
    update();
}

}  // namespace addu94::ui
