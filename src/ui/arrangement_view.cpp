// addu94 — src/ui/arrangement_view.cpp
#include "ui/arrangement_view.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace addu94::ui {

namespace {

constexpr QColor kBg{0x1a, 0x1d, 0x21};
constexpr QColor kPanel{0x23, 0x27, 0x2c};
constexpr QColor kBlue{0x4f, 0x9c, 0xf9};
constexpr QColor kGreen{0x37, 0xb2, 0x6c};
constexpr QColor kRed{0xd9, 0x53, 0x4f};
constexpr QColor kText{0xcf, 0xd6, 0xdd};
constexpr QColor kDim{0x6b, 0x74, 0x7e};
constexpr QColor kGrid{0x2e, 0x34, 0x3b};

constexpr int kTrackCount = 4;
constexpr int kRulerHeight = 24;

}  // namespace

ArrangementView::ArrangementView(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ArrangementView::set_blocks(std::vector<Block> blocks) {
    blocks_ = std::move(blocks);
    update();
}

void ArrangementView::refresh() {
    // Transport position → ticks (96 PPQ) → bars, same math as the
    // engine's own now_ticks() helper (see play_engine.cpp).
    auto const& ctrl = engine_.audio().transport_control();
    auto const stats = engine_.audio().stats();
    double const bpm = ctrl.tempo_bpm.load(std::memory_order_relaxed);
    double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
    double const ticks =
        static_cast<double>(engine_.audio().transport_position()) /
        (static_cast<double>(stats.sample_rate) * spb) * 96.0;
    double const beats_per_bar =
        static_cast<double>(ctrl.meter_num.load(std::memory_order_relaxed));
    playhead_bars_ = ticks / (96.0 * beats_per_bar);
    update();
}

int ArrangementView::first_visible_bar() const noexcept {
    // Window follows the playhead, keeping 2 bars of lead-in.
    int const first = static_cast<int>(std::floor(playhead_bars_)) - 2;
    return std::max(first, 0);
}

double ArrangementView::bar_width() const noexcept {
    return width() / 16.0;  // 16 bars visible
}

void ArrangementView::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), kBg);

    double const w = static_cast<double>(width());
    double const h = static_cast<double>(height());
    double const ruler = static_cast<double>(kRulerHeight);
    int const first_bar = first_visible_bar();
    double const bw = bar_width();

    // Ruler: bar numbers, 4-bar grid accent.
    p.fillRect(QRectF(0, 0, w, ruler), kPanel);
    p.setFont(QFont{font().family(), std::max(font().pointSize() - 2, 7)});
    for (int b = first_bar; b <= first_bar + 16; ++b) {
        double const x = (b - first_bar) * bw;
        p.setPen((b % 4 == 0) ? kDim : kGrid);
        p.drawLine(QPointF(x, 0), QPointF(x, h));
        if (b % 4 == 0) {
            p.setPen(kText);
            p.drawText(QRectF(x + 3, 0, bw, ruler),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(b + 1));
        }
    }

    // Track rows (engine's 4 default tracks).
    double const rows_h = h - ruler;
    double const row_h = rows_h / kTrackCount;
    int const selected = engine_.selected_track();
    for (int t = 0; t < kTrackCount; ++t) {
        QRectF const row(0, ruler + t * row_h, w, row_h);
        if (t == selected) {
            p.fillRect(row, QColor(0x23, 0x27, 0x2c));
        }
        p.setPen(kGrid);
        p.drawLine(QPointF(0, row.bottom()), QPointF(w, row.bottom()));
        p.setPen(t == selected ? kText : kDim);
        p.drawText(QRectF(row.left() + 4, row.top(), 60, row.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString("T%1").arg(t + 1));
    }

    // Pattern blocks (M4: the current clip as one block on the selected
    // track; M5 replaces this with the real arrangement).
    for (Block const& b : blocks_) {
        if (b.track < 0 || b.track >= kTrackCount) continue;
        double const x = (b.start_bar - first_bar) * bw;
        if (x > w) continue;
        QRectF const block(x, ruler + b.track * row_h + 4,
                           std::max(b.length_bars * bw, 6.0), row_h - 8);
        p.fillRect(block, QColor(0x4f, 0x9c, 0xf9, 0x40));
        p.setPen(QPen(kBlue, 1));
        p.drawRect(block);
    }

    // Click cursor (view-only; transport scrub lands in M5).
    {
        double const x = (cursor_bar_ - first_bar) * bw;
        if (x >= 0 && x <= w) {
            QPen dash(kGreen, 1, Qt::DashLine);
            p.setPen(dash);
            p.drawLine(QPointF(x, ruler), QPointF(x, h));
        }
        p.setPen(kDim);
        p.drawText(QRectF(w - 220, 0, 216, ruler),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("cursor: bar %1 (view only)").arg(cursor_bar_ + 1));
    }

    // Playhead.
    {
        double const x = (playhead_bars_ - first_bar) * bw;
        QPen ph(kRed, 2);
        p.setPen(ph);
        p.drawLine(QPointF(x, 0), QPointF(x, h));
    }
}

void ArrangementView::mousePressEvent(QMouseEvent* event) {
    // View-only cursor: shows the bar under the click. Transport scrubbing
    // (ScrubCoarse/ScrubFine actions) is documented as not wired yet — no
    // editing happens here until M5 lands the real arrangement.
    int const bar =
        first_visible_bar() + static_cast<int>(event->position().x() / bar_width());
    cursor_bar_ = std::max(bar, 0);
    update();
    QWidget::mousePressEvent(event);
}

}  // namespace addu94::ui
