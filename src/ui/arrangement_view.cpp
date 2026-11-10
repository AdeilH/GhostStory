// ghost_story — src/ui/arrangement_view.cpp
#include "ui/arrangement_view.hpp"

#include "ui/theme.hpp"

#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <QFontMetrics>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ghost_story::ui {

namespace {

using theme::with_alpha;

constexpr int kRulerHeight = 26;
constexpr int kVisibleBars = 32;      // bars shown at once (scrolls w/ playhead)
constexpr int kSectionLen = 8;        // intro/verse/chorus every 8 bars
constexpr double kPlayheadTrail = 24.0;
constexpr double kEdgeGrabPx = 6.0;   // block resize-edge hit width

// Section names/colors cycle every 8 bars (defaults: intro 0, verse 8,
// chorus 16, then repeat). Hues match theme::pal().track_hue so section bands,
// track chips and pattern blocks all speak the same color language.
struct Section {
    char const* name;
    int hue;
};
constexpr Section kSections[3] = {{"INTRO", 212}, {"VERSE", 165}, {"CHORUS", 25}};

std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
        "Arrangement canvas — every recorded take draws its own block.\n"
        "Click anywhere: move the playhead (play/record start there).\n"
        "Drag a block: move that instrument's recordings along the "
        "timeline (snaps to bars).\n"
        "Drag a block's right edge: resize that take.\n"
        "Double-click a block: extend that take +1 bar.\n"
        "Double-click an empty lane: reserve a clip window there.\n"
        "Right-click a block: clear that take's recording.\n"
        "Record (R) arms — the take starts when you PLAY, from the "
        "playhead, and lands as a new block.");
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(false);
}

void ArrangementView::set_track_clips(
    int track,
    std::vector<app::PlayEngine::ClipBoxView> const& boxes,
    int reserved_start_bar, int reserved_bars, bool reserved_has_notes) {
    if (track < 0 || track >= static_cast<int>(app::kTrackCount) ||
        reserved_bars <= 0) {
        return;
    }
    auto const ti = static_cast<std::size_t>(track);
    // Dedupe: MainWindow calls this every tick with engine truth; identical
    // feeds must not churn the block vector.
    if (fed_boxes_[ti] == boxes && fed_reserved_start_[ti] == reserved_start_bar &&
        fed_reserved_bars_[ti] == reserved_bars &&
        fed_reserved_notes_[ti] == reserved_has_notes) {
        return;
    }
    fed_boxes_[ti] = boxes;
    fed_reserved_start_[ti] = reserved_start_bar;
    fed_reserved_bars_[ti] = reserved_bars;
    fed_reserved_notes_[ti] = reserved_has_notes;

    // Rebuild this track's blocks: one per recorded take box; the
    // reserved window appears only while the track has no takes (a
    // legacy/replayed clip's events render through reserved_has_notes).
    auto write = blocks_.begin();
    for (Block const& b : blocks_) {
        if (b.track != track) *write++ = b;
    }
    blocks_.erase(write, blocks_.end());
    if (!boxes.empty()) {
        for (auto const& bx : boxes) {
            blocks_.push_back({track, bx.take, bx.start_bar, bx.bars,
                               bx.has_notes});
        }
    } else {
        blocks_.push_back({track, -1, reserved_start_bar, reserved_bars,
                           reserved_has_notes});
    }
    update();
}

void ArrangementView::refresh() {
    playhead_bars_ = position_bars(engine_);
    // Prune blocks of tracks that no longer exist — removing instruments
    // must leave NO dead blocks painted over the empty-canvas hint (the
    // stale-ghost bug: the old view kept them forever).
    int const n = engine_.track_count();
    auto write = blocks_.begin();
    for (Block const& b : blocks_) {
        if (b.track < n) *write++ = b;
    }
    bool const pruned = write != blocks_.end();
    blocks_.erase(write, blocks_.end());
    if (pruned) {
        // Reset the dedupe caches too: a slot may be re-added with a
        // state identical to the removed track's last feed, and a dedupe
        // hit would then skip the block the canvas just pruned.
        for (std::size_t t = 0; t < fed_boxes_.size(); ++t) {
            fed_boxes_[t].clear();
            fed_reserved_start_[t] = 0;
            fed_reserved_bars_[t] = 0;
            fed_reserved_notes_[t] = false;
        }
    }
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
    int const n = std::max(engine_.track_count(), 1);
    double const row_h = (height() - ruler) / static_cast<double>(n);
    // Placement: the block sits at its clip's start bar.
    return {(static_cast<double>(b.start_bar) -
             static_cast<double>(first_visible_bar())) * bw,
            ruler + b.track * row_h + 5.0,
            std::max(b.length_bars * bw, 6.0), row_h - 10.0};
}

int ArrangementView::edge_block_index_at(QPointF const& pos) const noexcept {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        QRectF const r = block_rect(blocks_[i]);
        if (pos.y() >= r.top() && pos.y() <= r.bottom() &&
            std::abs(pos.x() - r.right()) <= kEdgeGrabPx) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ArrangementView::block_index_at(QPointF const& pos) const noexcept {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (block_rect(blocks_[i]).contains(pos)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ArrangementView::block_index_of(int track, int take) const noexcept {
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].track == track && blocks_[i].take == take) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ArrangementView::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    double const w = static_cast<double>(width());
    double const h = static_cast<double>(height());
    double const ruler = static_cast<double>(kRulerHeight);
    int const first_bar = first_visible_bar();
    double const bw = bar_width();

    // Canvas: recessed gradient derived from the palette's well color —
    // the arrangement reads as the stage floor the blocks sit on, in
    // whatever depth the active theme defines.
    QLinearGradient canvas(QPointF(0, 0), QPointF(0, h));
    canvas.setColorAt(0.0, theme::shift(theme::pal().well, 5));
    canvas.setColorAt(1.0, theme::shift(theme::pal().well, -3));
    p.fillRect(rect(), canvas);

    auto x_of = [&](double bar) {
        return (bar - static_cast<double>(first_bar)) * bw;
    };

    // --- lanes (selected track tinted in its own hue, hairline separators) ---
    int const lane_count = std::max(engine_.track_count(), 0);
    double const rows_h = h - ruler;
    double const row_h =
        rows_h / static_cast<double>(std::max(lane_count, 1));
    int const selected = engine_.selected_track();
    for (int t = 0; t < lane_count; ++t) {
        QRectF const row(0, ruler + t * row_h, w, row_h);
        if (t == selected) {
            p.fillRect(row, with_alpha(theme::track_color(t), 0.07));
        }
        p.setPen(QPen(with_alpha(theme::pal().grid, 0.8), 1.0));
        p.drawLine(QPointF(0, row.bottom()), QPointF(w, row.bottom()));
    }

    // --- loop region shading (from the transport control plane) ---
    auto const& ctrl = engine_.audio().transport_control();
    std::uint64_t const loop_bars = ctrl.loop_bars.load(std::memory_order_relaxed);
    if (loop_bars > 0) {
        double const lx = x_of(0);
        double const lw = static_cast<double>(loop_bars) * bw;
        p.fillRect(QRectF(lx, ruler, lw, rows_h), with_alpha(theme::pal().accent, 0.06));
        // Bracket the loop region with two quiet accent posts.
        p.setPen(QPen(with_alpha(theme::pal().accent, 0.35), 1.0));
        p.drawLine(QPointF(lx + 0.5, ruler), QPointF(lx + 0.5, h));
        p.drawLine(QPointF(lx + lw - 0.5, ruler), QPointF(lx + lw - 0.5, h));
    }

    // --- bar grid + numbers (numbers sit under the section strip) ---
    QFont const bar_font{font().family(), 11};
    double constexpr kSectionStrip = 9.0;
    for (int b = first_bar; b <= first_bar + kVisibleBars; ++b) {
        double const x = x_of(static_cast<double>(b));
        bool const major = b % 4 == 0;
        p.setPen(QPen(major ? with_alpha(theme::pal().border, 0.9)
                            : with_alpha(theme::pal().grid, 0.6),
                      1.0));
        p.drawLine(QPointF(x, ruler), QPointF(x, h));
        if (b % 2 == 0) {  // number every 2 bars keeps the ruler calm
            p.setPen(major ? with_alpha(theme::pal().text, 0.85) : with_alpha(theme::pal().muted, 0.8));
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
        QColor const color = QColor::fromHsl(sec.hue, theme::pal().track_sat,
                                             theme::pal().track_light);
        double const x = x_of(static_cast<double>(s * kSectionLen));
        p.fillRect(QRectF(x, 0, kSectionLen * bw, kSectionStrip),
                   with_alpha(color, 0.42));
        p.setPen(with_alpha(theme::pal().text, 0.75));
        p.setFont(section_font);
        p.drawText(QRectF(x + 4, 0, kSectionLen * bw - 8, kSectionStrip),
                   Qt::AlignLeft | Qt::AlignVCenter, QString(sec.name));
    }
    p.setPen(QPen(theme::pal().border, 1.0));
    p.drawLine(QPointF(0, ruler), QPointF(w, ruler));

    // --- pattern blocks: gradient body (recorded) or faint outline
    // (reserved length, nothing recorded yet); a block live in the current
    // take wears a pulsing record glow + REC badge — the take is being
    // placed right now ---
    std::int64_t const now_ms = steady_ms();
    unsigned const rec_mask = engine_.recording_mask();
    bool const rec_live = engine_.recording();
    for (Block const& b : blocks_) {
        if (b.track < 0 || b.track >= static_cast<int>(app::kTrackCount)) {
            continue;
        }
        QRectF const r = block_rect(b);
        if (r.right() < 0 || r.left() > w) continue;
        QColor const hue = theme::track_color(b.track);
        bool const live =
            rec_live && ((rec_mask >> b.track) & 1u) != 0;

        if (b.has_notes) {
            theme::draw_shadow(p, r, 6.0);
            QLinearGradient grad(r.topLeft(), r.bottomLeft());
            grad.setColorAt(0.0, hue.lighter(130));
            grad.setColorAt(0.5, hue);
            grad.setColorAt(1.0, hue.darker(150));
            p.setPen(QPen(hue.lighter(150), 1.0));
            p.setBrush(grad);
            p.drawRoundedRect(r, 6.0, 6.0);
            // Top-edge highlight: light catching the top of the block.
            p.setPen(QPen(QColor(255, 255, 255, 34), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 5.0, 5.0);
            p.setPen(with_alpha(QColor(0, 0, 0), 0.75));
        } else if (b.take >= 0) {
            // A live take's provisional box (placed before the first note
            // lands): dashed but warm — the REC pulse below marks it as
            // the take in progress rather than reserved space.
            QLinearGradient faint(r.topLeft(), r.bottomLeft());
            faint.setColorAt(0.0, with_alpha(hue, 0.16));
            faint.setColorAt(1.0, with_alpha(hue, 0.08));
            p.setPen(QPen(with_alpha(hue, 0.70), 1.2, Qt::DashLine));
            p.setBrush(faint);
            p.drawRoundedRect(r, 6.0, 6.0);
            p.setPen(with_alpha(theme::pal().muted, 0.9));
        } else {
            // Empty clip: quiet dashed outline — "reserved space, hit
            // record" reads honestly instead of promising content.
            QLinearGradient faint(r.topLeft(), r.bottomLeft());
            faint.setColorAt(0.0, with_alpha(hue, 0.10));
            faint.setColorAt(1.0, with_alpha(hue, 0.05));
            p.setPen(QPen(with_alpha(hue, 0.55), 1.2, Qt::DashLine));
            p.setBrush(faint);
            p.drawRoundedRect(r, 6.0, 6.0);
            p.setPen(with_alpha(theme::pal().muted, 0.9));
        }

        if (live) {
            // The take is landing in this block right now: record-red
            // pulse over the body + a REC badge, so the growing edge reads
            // as recording rather than as a stray resize.
            double const pulse =
                0.5 + 0.5 * std::sin(static_cast<double>(now_ms) / 170.0);
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme::pal().record, 0.14 + 0.14 * pulse));
            p.drawRoundedRect(r, 6.0, 6.0);
            p.setPen(QPen(with_alpha(theme::pal().record,
                                     0.60 + 0.40 * pulse),
                          1.6));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 6.0, 6.0);
            QFont const badge_font{font().family(), 8, QFont::DemiBold};
            QFontMetrics const bfm{badge_font};
            double const tag_w = bfm.horizontalAdvance(
                                     QStringLiteral("REC")) + 10.0;
            QRectF const tag_r{r.right() - tag_w - 5.0, r.top() + 5.0,
                               tag_w, 14.0};
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme::pal().record, 0.85));
            p.drawRoundedRect(tag_r, 4, 4);
            p.setPen(theme::is_light() ? QColor(0xfa, 0xf8, 0xf4)
                                       : QColor(0x10, 0x14, 0x1a));
            p.setFont(badge_font);
            p.drawText(tag_r, Qt::AlignCenter, QStringLiteral("REC"));
        }

        p.setFont(QFont{font().family(), 10, QFont::DemiBold});
        // The name is read LIVE from the engine: after a removal compacts
        // the slots, a block must never wear a dead instrument's name.
        p.drawText(r.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(engine_.track_name(b.track)));
    }

    // --- drag ghosts (live previews; engine commits on release) ---
    if (resizing_ && resize_track_ >= 0) {
        int const at = block_index_of(resize_track_, resize_take_);
        if (at >= 0) {
            Block preview = blocks_[static_cast<std::size_t>(at)];
            preview.length_bars = resize_bars_;
            QRectF const r = block_rect(preview);
            p.setPen(QPen(theme::pal().accent, 1.5, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 6.0, 6.0);
        }
    }
    if (moving_ && move_track_ >= 0) {
        int const at = block_index_of(move_track_, move_take_);
        if (at >= 0) {
            // Ghost the whole group: every block of this track shifts by
            // the same delta (the engine commits the group move).
            int const delta = move_start_bar_ -
                              blocks_[static_cast<std::size_t>(at)].start_bar;
            for (Block const& b : blocks_) {
                if (b.track != move_track_) continue;
                Block preview = b;
                preview.start_bar += delta;
                QRectF const r = block_rect(preview);
                p.setPen(QPen(theme::pal().accent, 1.5, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r, 6.0, 6.0);
            }
        }
    }

    // --- playhead start marker (where play/record will begin) ---
    {
        // The transport playhead line below already follows playback; this
        // quiet marker shows the START position (the click target).
        auto const& tc = engine_.audio().transport_control();
        auto const stats = engine_.audio().stats();
        double const bpm = tc.tempo_bpm.load(std::memory_order_relaxed);
        double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
        double const start_bars =
            static_cast<double>(tc.start_position.load(
                std::memory_order_relaxed)) /
            (static_cast<double>(stats.sample_rate) * spb) / 96.0 /
            static_cast<double>(tc.meter_num.load(
                std::memory_order_relaxed));
        double const x = x_of(start_bars);
        if (x >= 0 && x <= w) {
            p.setPen(QPen(with_alpha(theme::pal().text, 0.35), 1.0,
                          Qt::DotLine));
            p.drawLine(QPointF(x, ruler), QPointF(x, h));
        }
    }

    // --- playhead: soft 24px trail, 2px accent line, glowing head ---
    {
        double const x = x_of(playhead_bars_);
        if (x >= -kPlayheadTrail && x <= w) {
            QRectF const trail(x - kPlayheadTrail, ruler, kPlayheadTrail, rows_h);
            QLinearGradient tg(trail.topLeft(), trail.topRight());
            tg.setColorAt(0.0, with_alpha(theme::pal().accent, 0.0));
            tg.setColorAt(1.0, with_alpha(theme::pal().accent, 0.35));
            p.fillRect(trail, tg);
            p.setPen(QPen(theme::pal().accent, 2.0));
            p.drawLine(QPointF(x, 0), QPointF(x, h));
            // Head: a hot dot with one soft halo at the ruler, so the eye
            // catches the beat position instantly. Clamped inside the
            // canvas — at position 0 the unclamped dot was half cut by the
            // edge and read as a red smear over the first bar number.
            double const hx = std::clamp(x, 6.0, w - 6.0);
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme::pal().accent_hot, 0.30));
            p.drawEllipse(QPointF(hx, ruler / 2.0), 6.0, 6.0);
            p.setBrush(theme::pal().accent_hot);
            p.drawEllipse(QPointF(hx, ruler / 2.0), 3.0, 3.0);
        }
    }

    // --- empty-canvas hint: the "how do I even start" answer, painted
    // right where the eyes go when nothing is recorded yet ---
    bool const all_empty = std::none_of(
        blocks_.begin(), blocks_.end(),
        [](Block const& b) { return b.has_notes; });
    if (lane_count == 0 && !moving_ && !resizing_) {
        p.setFont(theme::label_font(font()));
        p.setPen(with_alpha(theme::pal().muted, 0.9));
        // Word wrap: drawText clips to the rect, and at the window's
        // narrow floor the one-liner is wider than the canvas — it used
        // to clip mid-word on both sides.
        p.drawText(QRectF(0, ruler, w, rows_h),
                   Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap,
                   QStringLiteral("add an instrument (+, bottom left) — then "
                                  "press RECORD and play: the take starts "
                                  "when you play"));
    } else if (all_empty && !moving_ && !resizing_) {
        p.setFont(theme::label_font(font()));
        p.setPen(with_alpha(theme::pal().muted, 0.9));
        p.drawText(QRectF(0, ruler, w, rows_h),
                   Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap,
                   engine_.recording()
                       ? QStringLiteral("REC — play and the take lands "
                                        "here, growing under the playhead "
                                        "· stop keeps it")
                       : QStringLiteral("press RECORD and play — the take "
                                        "starts when you play, from the "
                                        "playhead"));
    }

    // Panel border on top (grid lines run to the edges beneath it).
    p.setPen(QPen(theme::pal().border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                      theme::kPanelRadius, theme::kPanelRadius);
}

void ArrangementView::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    // Right-click a block: clear THAT take's recording (the "remove after
    // recording" ask — the track and its other takes survive). The
    // reserved window clears the whole track, as before.
    if (event->button() == Qt::MouseButton::RightButton) {
        int const at = block_index_at(pos);
        if (at >= 0) {
            Block const& b = blocks_[static_cast<std::size_t>(at)];
            QMenu menu(this);
            QString const name = QString::fromUtf8(engine_.track_name(b.track));
            QAction* clear;
            if (b.take >= 0) {
                clear = menu.addAction(
                    QString("Clear this take (\"%1\" #%2)")
                        .arg(name, QString::number(b.take + 1)));
            } else {
                clear = menu.addAction(
                    QString("Clear recording (\"%1\")").arg(name));
            }
            if (menu.exec(event->globalPosition().toPoint()) == clear) {
                if (b.take >= 0) {
                    engine_.clear_take(b.track, b.take);
                } else {
                    engine_.clear_clip(b.track);
                }
            }
            update();
            return;
        }
        QWidget::mousePressEvent(event);
        return;
    }

    // Block edge: start a resize drag — THAT take's box, length commits
    // on release.
    int const edge = edge_block_index_at(pos);
    if (edge >= 0) {
        Block const& b = blocks_[static_cast<std::size_t>(edge)];
        resizing_ = true;
        resize_track_ = b.track;
        resize_take_ = b.take;
        resize_bars_ = b.length_bars;
        return;
    }

    // Block body: start a MOVE drag — the track's whole take-group
    // relocates ("record a portion and drop it in the play bar"). Commits
    // through the engine on release, snapped to whole bars.
    int const at = block_index_at(pos);
    if (at >= 0) {
        Block const& b = blocks_[static_cast<std::size_t>(at)];
        moving_ = true;
        move_track_ = b.track;
        move_take_ = b.take;
        move_box_start_ = b.start_bar;
        move_start_bar_ = b.start_bar;
        move_window_start_ = engine_.clip_start_bar(b.track);
        move_grab_offset_ = (pos.x() - block_rect(b).left()) / bar_width();
        return;
    }

    // Otherwise: the click IS the playhead — play/record start there.
    engine_.set_playhead_bar(std::max(bar_at(pos.x()), 0));
    update();
    QWidget::mousePressEvent(event);
}

void ArrangementView::mouseMoveEvent(QMouseEvent* event) {
    if (resizing_ && resize_track_ >= 0) {
        double const bar_f = static_cast<double>(first_visible_bar()) +
                             event->position().x() / bar_width();
        resize_bars_ = std::clamp(static_cast<int>(std::lround(bar_f)), 1, 64);
        update();
        return;
    }
    if (moving_ && move_track_ >= 0) {
        double const bar_f = static_cast<double>(first_visible_bar()) +
                             event->position().x() / bar_width();
        move_start_bar_ =
            std::max(0, static_cast<int>(std::lround(bar_f - move_grab_offset_)));
        update();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ArrangementView::mouseReleaseEvent(QMouseEvent* event) {
    if (resizing_ && resize_track_ >= 0) {
        // Engine commit — the box is the take, so the length change is
        // real the moment this lands (the next tick feeds it back).
        if (resize_take_ >= 0) {
            engine_.set_box_bars(resize_track_, resize_take_, resize_bars_);
        } else {
            engine_.set_clip_bars(resize_track_, resize_bars_);
        }
        resizing_ = false;
        resize_track_ = -1;
        resize_take_ = -1;
        update();
        return;
    }
    if (moving_ && move_track_ >= 0) {
        // The grabbed box leads the group: the window start shifts by the
        // box's delta so every take and note moves together.
        engine_.set_clip_start(move_track_,
                               move_window_start_ + move_start_bar_ -
                                   move_box_start_);
        moving_ = false;
        move_track_ = -1;
        move_take_ = -1;
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ArrangementView::mouseDoubleClickEvent(QMouseEvent* event) {
    QPointF const pos = event->position();
    double const ruler = static_cast<double>(kRulerHeight);
    int const n = engine_.track_count();
    if (n <= 0) return;
    double const row_h = (height() - ruler) / static_cast<double>(n);
    int const lane = static_cast<int>((pos.y() - ruler) / row_h);
    if (lane < 0 || lane >= n) return;

    // On a block: extend THAT take +1 bar (reserved window: the clip
    // window). On an empty lane: a 1-bar reserved window at the clicked
    // bar (the playhead click model makes this the natural spot).
    int const at = block_index_at(pos);
    if (at >= 0) {
        Block const& b = blocks_[static_cast<std::size_t>(at)];
        if (b.take >= 0) {
            engine_.set_box_bars(b.track, b.take, b.length_bars + 1);
        } else {
            engine_.set_clip_bars(b.track, b.length_bars + 1);
        }
        update();
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    engine_.set_clip_bars(lane, 1);
    engine_.set_clip_start(lane, std::max(bar_at(pos.x()), 0));
    update();
    QWidget::mouseDoubleClickEvent(event);
}

}  // namespace ghost_story::ui
