// addu94 — src/ui/arrangement_view.cpp
#include "ui/arrangement_view.hpp"

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
using theme::kPanel;
using theme::kBorder;
using theme::kGrid;
using theme::kMuted;
using theme::kText;
using theme::with_alpha;

constexpr int kTrackCount = 4;
constexpr int kRulerHeight = 26;
constexpr int kVisibleBars = 32;      // bars shown at once (scrolls w/ playhead)
constexpr int kSectionLen = 8;        // intro/verse/chorus every 8 bars
constexpr double kPlayheadTrail = 24.0;

// Section names/colors cycle every 8 bars (defaults: intro 0, verse 8,
// chorus 16, then repeat). Colors reuse the desaturated track hues.
struct Section {
    char const* name;
    int hue;
};
constexpr Section kSections[3] = {{"INTRO", 210}, {"VERSE", 160}, {"CHORUS", 30}};

double position_bars(app::PlayEngine& engine) noexcept {
    // Transport position (samples) → ticks (96 PPQ) → bars. Same math as
    // play_engine.cpp's now_ticks() helper.
    auto const& ctrl = engine.audio().transport_control();
    auto const stats = engine.audio().stats();
    double const bpm = ctrl.tempo_bpm.load(std::memory_order_relaxed);
    double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
    double const ticks =
        static_cast<double>(engine.audio().transport_position()) /
        (static_cast<double>(stats.sample_rate) * spb) * 96.0;
    double const beats_per_bar =
        static_cast<double>(ctrl.meter_num.load(std::memory_order_relaxed));
    return ticks / (96.0 * beats_per_bar);
}

}  // namespace

ArrangementView::ArrangementView(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip(
        "Arrangement canvas — click: move cursor; drag a block: move it "
        "(snaps to bars); double-click an empty lane: extend/create the "
        "clip block");
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(false);
}

void ArrangementView::set_clip_block(int track, int length_bars) {
    // Dedupe: MainWindow calls this every tick with the engine's current
    // clip; identical calls must not reset a user drag / extension.
    if (track == 0 && length_bars == clip_len_bars_) {
        if (!blocks_.empty()) return;
    }
    clip_len_bars_ = length_bars;
    ensure_block(track, length_bars);
    update();
}

void ArrangementView::ensure_block(int track, int length_bars) {
    if (length_bars <= 0) return;
    for (Block& b : blocks_) {
        if (b.track == track && b.name == QStringLiteral("Clip 1")) {
            b.length_bars = length_bars;
            return;
        }
    }
    blocks_.push_back({track, clip_start_bar_, length_bars,
                       QStringLiteral("Clip 1")});
}

void ArrangementView::refresh() {
    playhead_bars_ = position_bars(engine_);
    update();
}

int ArrangementView::first_visible_bar() const noexcept {
    // Window follows the playhead, keeping 2 bars of lead-in.
    int const first = static_cast<int>(std::floor(playhead_bars_)) - 2;
    return std::max(first, 0);
}

double ArrangementView::bar_width() const noexcept {
    return static_cast<double>(width()) / static_cast<double>(kVisibleBars);
}

int ArrangementView::bar_at(double x) const noexcept {
    double const bw = bar_width();
    if (bw <= 0.0) return 0;
    return first_visible_bar() + static_cast<int>(std::floor(x / bw));
}

QRectF ArrangementView::block_rect(Block const& b) const noexcept {
    double const bw = bar_width();
    double const ruler = static_cast<double>(kRulerHeight);
    double const row_h = (height() - ruler) / static_cast<double>(kTrackCount);
    double const x = (static_cast<double>(b.start_bar) -
                      static_cast<double>(first_visible_bar())) * bw;
    return {x, ruler + b.track * row_h + 5.0,
            std::max(b.length_bars * bw, 6.0), row_h - 10.0};
}

void ArrangementView::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    double const w = static_cast<double>(width());
    double const h = static_cast<double>(height());
    double const ruler = static_cast<double>(kRulerHeight);
    int const first_bar = first_visible_bar();
    double const bw = bar_width();

    p.fillRect(rect(), kPanel);

    auto x_of = [&](double bar) {
        return (bar - static_cast<double>(first_bar)) * bw;
    };

    // --- lanes (selected track highlighted) ---
    double const rows_h = h - ruler;
    double const row_h = rows_h / static_cast<double>(kTrackCount);
    int const selected = engine_.selected_track();
    for (int t = 0; t < kTrackCount; ++t) {
        QRectF const row(0, ruler + t * row_h, w, row_h);
        if (t == selected) {
            p.fillRect(row, with_alpha(kAccent, 0.05));
        }
        p.setPen(QPen(kGrid, 1.0));
        p.drawLine(QPointF(0, row.bottom()), QPointF(w, row.bottom()));
    }

    // --- loop region shading (from the transport control plane) ---
    auto const& ctrl = engine_.audio().transport_control();
    std::uint64_t const loop_bars = ctrl.loop_bars.load(std::memory_order_relaxed);
    if (loop_bars > 0) {
        p.fillRect(QRectF(x_of(0), ruler,
                          static_cast<double>(loop_bars) * bw, rows_h),
                   with_alpha(kAccent, 0.07));
    }

    // --- bar grid + numbers (numbers sit under the section strip) ---
    QFont const bar_font{font().family(), 11};
    double constexpr kSectionStrip = 9.0;
    for (int b = first_bar; b <= first_bar + kVisibleBars; ++b) {
        double const x = x_of(static_cast<double>(b));
        bool const major = b % 4 == 0;
        p.setPen(QPen(major ? kBorder : kGrid, 1.0));
        p.drawLine(QPointF(x, ruler), QPointF(x, h));
        if (b % 2 == 0) {  // number every 2 bars keeps the ruler calm
            p.setPen(major ? kText : kMuted);
            p.setFont(bar_font);
            p.drawText(QRectF(x + 4, kSectionStrip, bw, ruler - kSectionStrip),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(b + 1));
        }
    }

    // --- section strip (top 9px of the ruler): intro/verse/chorus bands ---
    QFont const section_font{font().family(), 7};
    for (int s = first_bar / kSectionLen;
         s <= (first_bar + kVisibleBars) / kSectionLen; ++s) {
        Section const& sec = kSections[s % 3];
        QColor const color = QColor::fromHsl(sec.hue, 153, 160);
        double const x = x_of(static_cast<double>(s * kSectionLen));
        p.fillRect(QRectF(x, 0, kSectionLen * bw, kSectionStrip),
                   with_alpha(color, 0.55));
        p.setPen(with_alpha(kText, 0.9));
        p.setFont(section_font);
        p.drawText(QRectF(x + 4, 0, kSectionLen * bw - 8, kSectionStrip),
                   Qt::AlignLeft | Qt::AlignVCenter, QString(sec.name));
    }
    p.setPen(QPen(kBorder, 1.0));
    p.drawLine(QPointF(0, ruler), QPointF(w, ruler));

    // --- pattern blocks ---
    for (Block const& b : blocks_) {
        if (b.track < 0 || b.track >= kTrackCount) continue;
        QRectF const r = block_rect(b);
        if (r.right() < 0 || r.left() > w) continue;
        QColor const hue = theme::track_color(b.track);

        theme::draw_shadow(p, r, 6.0);
        QLinearGradient grad(r.topLeft(), r.bottomLeft());
        grad.setColorAt(0.0, hue.lighter(125));
        grad.setColorAt(1.0, hue.darker(140));
        p.setPen(QPen(hue.lighter(150), 1.0));
        p.setBrush(grad);
        p.drawRoundedRect(r, 6.0, 6.0);

        p.setPen(with_alpha(QColor(0, 0, 0), 0.75));
        p.setFont(QFont{font().family(), 10, QFont::DemiBold});
        p.drawText(r.adjusted(8, 0, -8, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, b.name);
    }

    // --- drag ghost outline (snapped position preview) ---
    if (dragging_ && drag_index_ >= 0 &&
        drag_index_ < static_cast<int>(blocks_.size())) {
        Block preview = blocks_[static_cast<std::size_t>(drag_index_)];
        preview.start_bar = static_cast<int>(std::round(drag_pos_));
        QRectF const r = block_rect(preview);
        p.setPen(QPen(kAccent, 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 6.0, 6.0);
    }

    // --- click cursor ---
    {
        double const x = x_of(static_cast<double>(cursor_bar_));
        if (x >= 0 && x <= w) {
            p.setPen(QPen(with_alpha(kText, 0.45), 1.0, Qt::DashLine));
            p.drawLine(QPointF(x, ruler), QPointF(x, h));
        }
    }

    // --- playhead: soft 24px trail + 2px accent line ---
    {
        double const x = x_of(playhead_bars_);
        if (x >= -kPlayheadTrail && x <= w) {
            QRectF const trail(x - kPlayheadTrail, ruler, kPlayheadTrail, rows_h);
            QLinearGradient tg(trail.topLeft(), trail.topRight());
            tg.setColorAt(0.0, with_alpha(kAccent, 0.0));
            tg.setColorAt(1.0, with_alpha(kAccent, 0.35));
            p.fillRect(trail, tg);
            p.setPen(QPen(kAccent, 2.0));
            p.drawLine(QPointF(x, 0), QPointF(x, h));
        }
    }

    // Panel border on top (grid lines run to the edges beneath it).
    p.setPen(QPen(kBorder, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                      theme::kPanelRadius, theme::kPanelRadius);
}

void ArrangementView::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    // Block drag start (horizontal move; snap to whole bars on release).
    for (std::size_t i = blocks_.size(); i-- > 0;) {
        if (block_rect(blocks_[i]).contains(pos)) {
            dragging_ = true;
            drag_index_ = static_cast<int>(i);
            double const bw = bar_width();
            drag_grab_offset_ =
                (pos.x() - block_rect(blocks_[i]).left()) / bw;
            drag_pos_ = static_cast<double>(blocks_[i].start_bar);
            return;
        }
    }

    // Otherwise: move the bar cursor.
    cursor_bar_ = std::max(bar_at(pos.x()), 0);
    update();
    QWidget::mousePressEvent(event);
}

void ArrangementView::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || drag_index_ < 0 ||
        drag_index_ >= static_cast<int>(blocks_.size())) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    double const bar_f =
        static_cast<double>(first_visible_bar()) +
        event->position().x() / bar_width();
    drag_pos_ = std::max(0.0, bar_f - drag_grab_offset_);
    update();
}

void ArrangementView::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && drag_index_ >= 0 &&
        drag_index_ < static_cast<int>(blocks_.size())) {
        Block& b = blocks_[static_cast<std::size_t>(drag_index_)];
        b.start_bar = std::max(0, static_cast<int>(std::round(drag_pos_)));
        clip_start_bar_ = b.start_bar;  // view state; engine wiring lands M5
    }
    dragging_ = false;
    drag_index_ = -1;
    update();
    QWidget::mouseReleaseEvent(event);
}

void ArrangementView::mouseDoubleClickEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    // Double-click on a block is reserved for future pattern editing.
    for (Block const& b : blocks_) {
        if (block_rect(b).contains(pos)) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }
    }

    // Empty lane: extend that lane's clip block +1 bar, or create a 1-bar
    // block at the clicked bar. VIEW STATE ONLY — engine arrangement
    // editing lands with M5.
    double const ruler = static_cast<double>(kRulerHeight);
    double const row_h = (height() - ruler) / static_cast<double>(kTrackCount);
    int const lane = static_cast<int>((pos.y() - ruler) / row_h);
    if (lane < 0 || lane >= kTrackCount) return;

    int const bar = std::max(bar_at(pos.x()), 0);
    for (Block& b : blocks_) {
        if (b.track == lane) {
            b.length_bars += 1;  // view-state extension
            update();
            return;
        }
    }
    blocks_.push_back({lane, bar, 1, QStringLiteral("Clip 1")});
    update();
}

}  // namespace addu94::ui
