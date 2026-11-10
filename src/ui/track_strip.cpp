// ghost_story — src/ui/track_strip.cpp
#include "ui/track_strip.hpp"

#include "ui/theme.hpp"

#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ghost_story::ui {

namespace {

using theme::with_alpha;

constexpr int kTrackCount = 4;

QString pan_label(int pan) {
    if (pan == 0) return QStringLiteral("C");
    return QString("%1%2").arg(pan < 0 ? QLatin1Char('L') : QLatin1Char('R'))
        .arg(std::abs(pan));
}

// Inset toggle (mute / arm): recessed well when off, tinted fill + colored
// border when on.
void paint_toggle(QPainter& p, QRectF const& r, QString const& glyph,
                  bool on, QColor const& tint, QFont const& font) {
    QLinearGradient fill{r.topLeft(), r.bottomLeft()};
    if (on) {
        fill.setColorAt(0.0, with_alpha(tint, 0.42));
        fill.setColorAt(1.0, with_alpha(tint, 0.20));
        p.setPen(QPen(with_alpha(tint, 0.9), 1.0));
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

TrackStrips::TrackStrips(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip(
        "Tracks — click a row to select it (MPK pads/keys follow). "
        "M mutes the track (tails keep decaying), the dot record-arms it "
        "for the loop recorder (nothing armed = everything records); "
        "drag the thin bar for volume (0..100%), drag the pan lane to "
        "set stereo position, double-click resets a lane");
    setMinimumWidth(244);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void TrackStrips::set_track_hook(TrackHook hook) { hook_ = std::move(hook); }

void TrackStrips::refresh() {
    // The engine atomics are the mixer truth; mirror them for painting so
    // hardware knob edits (selected-track volume/pan) show up instantly.
    unsigned const arm = engine_.record_arm_mask();
    int const n = engine_.track_count();
    for (int t = 0; t < n; ++t) {
        auto const ti = static_cast<std::size_t>(t);
        volume_[ti] = std::clamp(
            static_cast<int>(
                std::lround(static_cast<double>(engine_.track_volume(t)) *
                            100.0)),
            0, 100);
        pan_[ti] = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(
                                             engine_.track_pan(t)) *
                                             200.0 -
                                         100.0)),
            -100, 100);
        mute_[ti] = engine_.track_mute(t);
        arm_[ti] = ((arm >> t) & 1u) != 0;
    }
    update();
}

TrackStrips::RowGeom TrackStrips::row_geom(int track) const noexcept {
    double const w = static_cast<double>(width());
    double const h = static_cast<double>(height());
    double const pad = 8.0;
    // Dynamic rows + a fixed 26px "+ add instrument" bar at the bottom
    // (hidden when the pool is full — then the rows get the space back).
    int const n = std::max(engine_.track_count(), 0);
    double const rows_h = h - 2.0 * pad - add_bar_h();
    double const row_h = rows_h / static_cast<double>(std::max(n, 1));
    double const y = pad + static_cast<double>(track) * row_h;

    RowGeom g;
    g.full = {pad + 2.0, y + 3.0, w - 2.0 * (pad + 2.0), row_h - 6.0};
    g.chip = QRectF{g.full.left() + 4, g.full.top() + 6, 26, 22};

    double const right = g.full.right();
    // 4px inset to the card border (arm used to sit exactly ON the border).
    g.mute = QRectF{right - 64.0, g.full.top() + 6.0, 28.0, 24.0};
    g.arm = QRectF{right - 32.0, g.full.top() + 6.0, 28.0, 24.0};

    // Mixer lanes: centered in the space under the name/role block,
    // clamped so bar + label always fit inside the card. The old fixed
    // +42 offset pushed the "vol 100%" label past the card edge whenever
    // the window neared its floor — visible as a clipped caption on the
    // Bass card. Centering also keeps the four rows reading as one aligned
    // column at every window height, instead of hugging the top.
    double const lane_y = g.full.top() + 34.0 +
                          std::max(0.0, (g.full.height() - 58.0) / 2.0);
    // Lanes sit 11px inside the card: their wells draw 3px beyond the lane
    // (draw_well callers adjust ±3), so the visible well keeps an 8px inset
    // to the card border. The old offsets let the volume well land 1px from
    // the left border — flush with the edge, the fill read as a bar sliced
    // off by the card ("the Vol is cut"). Volume still ends 24px before the
    // pan lane (the old 8px gap made the two wells read as ONE long slider
    // — at vol 100% the handle looked parked at 75% of "the lane", which
    // read as broken, not as two controls).
    constexpr double kLaneInset = 11.0;
    g.vol = QRectF{g.full.left() + kLaneInset, lane_y,
                   g.full.width() - 2.0 * kLaneInset - 40.0 - 24.0, 16.0};
    g.pan = QRectF{g.full.right() - kLaneInset - 40.0, lane_y, 40.0, 16.0};
    return g;
}

void TrackStrips::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    theme::draw_panel(p, QRectF(rect()));

    QFont const name_font = theme::value_font(font(), 10);
    QFont const tiny_font{font().family(), 9};
    QFont const micro = theme::label_font(font());

    int const selected = engine_.selected_track();
    int const n = engine_.track_count();
    for (int t = 0; t < n; ++t) {
        RowGeom const g = row_geom(t);
        QColor const hue = theme::track_color(t);
        QColor const role_hue = theme::kind_color(
            static_cast<int>(engine_.track_kind(t)));

        // Row card: quiet glass for idle tracks; the selected track gets an
        // accent frame, a faint accent wash and a track-hue edge glow so it
        // reads instantly from across the room.
        QLinearGradient card{g.full.topLeft(), g.full.bottomLeft()};
        if (t == selected) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(hue, 0.12));
            p.drawRoundedRect(g.full.adjusted(-2.5, -2.5, 2.5, 2.5), 10, 10);
            card.setColorAt(0.0, with_alpha(theme::pal().accent, 0.10));
            card.setColorAt(1.0, with_alpha(theme::pal().accent, 0.04));
            p.setPen(QPen(with_alpha(theme::pal().accent, 0.55), 1.0));
        } else {
            card.setColorAt(0.0, theme::shift(theme::pal().panel_top, 6));
            card.setColorAt(1.0, theme::shift(theme::pal().panel_top, -2));
            p.setPen(QPen(QColor(0, 0, 0, theme::is_light() ? 40 : 90), 1.0));
        }
        p.setBrush(card);
        p.drawRoundedRect(g.full, 8, 8);
        // Inner top highlight on the card (dark themes only — light
        // surfaces don't need a fake sheen).
        if (!theme::is_light()) {
            p.setPen(QPen(QColor(255, 255, 255, 10), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(g.full.adjusted(1, 1, -1, -1), 7, 7);
        }

        // Hue chip + engine track name + kind tag. WHY the kind tag: keys/
        // pads route by kind, so "SYNTH / DRUM KIT" under the name tells
        // the player where input will land without opening menus.
        p.setPen(Qt::NoPen);
        QLinearGradient chip_fill{g.chip.topLeft(), g.chip.bottomLeft()};
        chip_fill.setColorAt(0.0, hue.lighter(theme::is_light() ? 110 : 120));
        chip_fill.setColorAt(1.0, hue.darker(theme::is_light() ? 110 : 130));
        p.setBrush(chip_fill);
        p.drawRoundedRect(g.chip, 5, 5);
        p.setPen(theme::is_light() ? QColor(0xfa, 0xf8, 0xf4)
                                   : QColor(0x10, 0x14, 0x1a));
        p.setFont(name_font);
        p.drawText(g.chip, Qt::AlignCenter, QString("T%1").arg(t + 1));

        p.setPen(t == selected ? theme::pal().text : with_alpha(theme::pal().text, 0.85));
        p.setFont(name_font);
        // 118px: everything up to the M toggle (right - 60) minus a breath
        // — the old 92px box elided half the track names at rail width.
        // Height from the font: the 10pt line box is ~15.6px and drawText
        // clips to the rect — a hardcoded 15 shaved descenders (y in
        // "Lo-Fi Keys") by a pixel on some font resolutions.
        p.drawText(QRectF{g.chip.right() + 8.0, g.chip.top(), 118.0, 17.0},
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(engine_.track_name(t)));
        p.setPen(role_hue);
        p.setFont(micro);
        // Height from the font: drawText clips to the rect, and a box
        // shorter than the line box shaves the glyph bottoms (the old
        // 10px box cut SYNTH / DRUM KIT; 14 still grazed on font
        // resolutions whose baseline landed 1px lower).
        p.drawText(QRectF{g.chip.right() + 8.0, g.chip.top() + 15.0, 118.0, 15.0},
                   Qt::AlignLeft | Qt::AlignTop,
                   QString::fromUtf8(theme::kind_label(
                       static_cast<int>(engine_.track_kind(t)))));

        // Mute / arm toggles.
        paint_toggle(p, g.mute, QStringLiteral("M"),
                     mute_[static_cast<std::size_t>(t)], theme::pal().muted, name_font);
        bool const armed = arm_[static_cast<std::size_t>(t)];
        paint_toggle(p, g.arm, QString(), armed, theme::pal().record, name_font);
        p.setPen(Qt::NoPen);
        p.setBrush(armed ? theme::pal().record : with_alpha(theme::pal().muted, 0.7));
        double const dot = 8.0;
        p.drawEllipse(QPointF{g.arm.center().x(), g.arm.center().y()},
                      dot / 2.0, dot / 2.0);

        // Volume: recessed lane, hue-gradient fill, draggable handle.
        QRectF const bar{g.vol.left(), g.vol.top() + 6.0, g.vol.width(), 4.0};
        theme::draw_well(p, bar.adjusted(-3.0, -3.0, 3.0, 3.0), 4.0);
        double const frac = static_cast<double>(
                                    volume_[static_cast<std::size_t>(t)]) / 100.0;
        // Handle travel keeps the dot fully inside the lane (5px radius):
        // at 100% the well's rounded cap stays visible beyond the fill, so
        // the slider reads as a complete track with the handle at max — not
        // as a bar chopped off by the card edge.
        double const hx = bar.left() + 5.0 +
                          (bar.width() - 10.0) * std::clamp(frac, 0.0, 1.0);
        double const fill_w = hx - bar.left();
        QLinearGradient vol_fill{bar.topLeft(), bar.bottomLeft()};
        vol_fill.setColorAt(0.0, hue.lighter(135));
        vol_fill.setColorAt(1.0, hue);
        p.setPen(Qt::NoPen);
        p.setBrush(vol_fill);
        p.drawRoundedRect(QRectF{bar.left(), bar.top(), fill_w, bar.height()},
                          2, 2);
        p.setBrush(theme::pal().text);
        p.drawEllipse(QPointF{hx, bar.center().y()}, 5.0, 5.0);
        p.setPen(theme::pal().muted);
        p.setFont(tiny_font);
        // Height from the font's own metrics — the old hardcoded 11px box
        // was shorter than the 9pt line box, and drawText clips to the
        // rect: on font resolutions whose baseline landed 1px lower the
        // bottoms of "vol 100%" were sliced off ("vol not showing
        // entirely").
        p.drawText(QRectF{bar.left(), bar.bottom() + 2.0, bar.width(),
                          static_cast<double>(QFontMetrics{tiny_font}.height()) + 2.0},
                   Qt::AlignLeft | Qt::AlignTop,
                   QString("vol %1%").arg(volume_[static_cast<std::size_t>(t)]));

        // Pan: recessed lane, center tick, position dot + label.
        QRectF const lane{g.pan.left(), g.pan.top() + 6.0, g.pan.width(), 4.0};
        theme::draw_well(p, lane.adjusted(-3.0, -3.0, 3.0, 3.0), 4.0);
        double const mid = lane.center().x();
        p.setBrush(theme::pal().border);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF{mid - 0.75, lane.top() - 3.0, 1.5,
                                 lane.height() + 6.0},
                          1, 1);
        // Clamp: pan_ is written clamped (-100..100) everywhere, but the
        // dot must never be able to leave the lane even if a future write
        // path forgets — a detached handle reads as a broken slider.
        double const pfrac = std::clamp(
            static_cast<double>(pan_[static_cast<std::size_t>(t)]) / 100.0,
            -1.0, 1.0);
        // Same inset travel as the volume handle: the dot stays fully
        // inside the lane, its position always matching the drag math.
        double const px = lane.left() + 4.0 +
                          (lane.width() - 8.0) * (pfrac + 1.0) / 2.0;
        p.setBrush(theme::pal().text);
        p.drawEllipse(QPointF{px, lane.center().y()}, 4.0, 4.0);
        p.setPen(theme::pal().muted);
        p.drawText(QRectF{lane.left() - 6.0, lane.bottom() + 2.0,
                          lane.width() + 12.0,
                          static_cast<double>(QFontMetrics{tiny_font}.height()) + 2.0},
                   Qt::AlignHCenter | Qt::AlignTop,
                   pan_label(pan_[static_cast<std::size_t>(t)]));
    }

    // "+ add instrument" bar: the visible dropdown affordance for the
    // dynamic track model — everything starts empty and grows from here.
    if (QRectF const bar = add_bar_rect(); !bar.isNull()) {
        QLinearGradient fill{bar.topLeft(), bar.bottomLeft()};
        fill.setColorAt(0.0, theme::shift(theme::pal().panel_top, 6));
        fill.setColorAt(1.0, theme::shift(theme::pal().panel_top, -2));
        p.setPen(QPen(with_alpha(theme::pal().accent, 0.55), 1.0, Qt::DashLine));
        p.setBrush(fill);
        p.drawRoundedRect(bar, 8, 8);
        p.setPen(theme::is_light() ? theme::pal().accent
                                   : theme::pal().accent_hot);
        p.setFont(theme::value_font(font(), 10));
        p.drawText(bar, Qt::AlignCenter, QStringLiteral("+ add instrument"));
    }
    if (engine_.track_count() == 0) {
        p.setFont(theme::label_font(font()));
        p.setPen(with_alpha(theme::pal().muted, 0.9));
        p.drawText(QRectF(rect()).adjusted(8, 8, -8, -40),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   QStringLiteral("no instruments yet"));
    }
}

void TrackStrips::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    // Right-click a row: remove that track (the add bar adds; this is the
    // removal half of "I will add them manually as needed").
    if (event->button() == Qt::MouseButton::RightButton) {
        int const n = engine_.track_count();
        for (int t = 0; t < n; ++t) {
            if (row_geom(t).full.adjusted(-4.0, -4.0, 4.0, 4.0)
                    .contains(pos)) {
                QMenu menu(this);
                QAction* remove = menu.addAction(
                    QString("Remove \"%1\"").arg(engine_.track_name(t)));
                QAction* chosen = menu.exec(event->globalPosition().toPoint());
                if (chosen == remove) {
                    std::string error;
                    if (!engine_.remove_track(t, error)) {
                        qWarning("remove track failed: %s", error.c_str());
                    }
                }
                update();
                return;
            }
        }
        QWidget::mousePressEvent(event);
        return;
    }

    // "+ add instrument" bar: the dropdown of everything that can be
    // added — the ten synth instruments (preset templates: patch + FX
    // chain preloaded) plus the shared Drum Kit.
    if (add_bar_rect().contains(pos)) {
        QMenu menu(this);
        std::vector<QAction*> preset_actions;
        for (int i = 0; i < static_cast<int>(engine_.presets().size()); ++i) {
            preset_actions.push_back(menu.addAction(
                QString::fromStdString(engine_.presets()[i].name)));
        }
        menu.addSeparator();
        QAction* drums = menu.addAction(QStringLiteral("Drum Kit"));
        if (engine_.drums_present()) {
            drums->setEnabled(false);  // the kit is shared — one track only
        }
        QAction* chosen = menu.exec(event->globalPosition().toPoint());
        if (chosen != nullptr) {
            std::string error;
            int const idx = chosen == drums
                                ? -1
                                : static_cast<int>(std::find(
                                      preset_actions.begin(),
                                      preset_actions.end(), chosen) -
                                  preset_actions.begin());
            if (!engine_.add_track(idx, error)) {
                qWarning("add track failed: %s", error.c_str());
            }
        }
        update();
        return;
    }

    int const n_tracks = engine_.track_count();
    for (int t = 0; t < n_tracks; ++t) {
        RowGeom const g = row_geom(t);
        if (!g.full.adjusted(-4.0, -4.0, 4.0, 4.0).contains(pos)) continue;

        if (g.mute.contains(pos)) {
            bool const on = !engine_.track_mute(t);
            engine_.set_track_mute(t, on);
            if (hook_) hook_(t, "mute", on ? 1.0 : 0.0);
            update();
            return;
        }
        if (g.arm.contains(pos)) {
            bool const on = !arm_[static_cast<std::size_t>(t)];
            engine_.set_record_arm(t, on);
            if (hook_) hook_(t, "arm", on ? 1.0 : 0.0);
            update();
            return;
        }
        if (g.vol.adjusted(-6.0, -8.0, 6.0, 8.0).contains(pos)) {
            drag_track_ = t;
            drag_volume_ = true;
            // Same inset travel the handle draws with (dot radius 5), so
            // the click position, the value and the dot always agree.
            double const rel = (pos.x() - g.vol.left() - 5.0) /
                               (g.vol.width() - 10.0);
            set_volume(t, std::clamp(rel, 0.0, 1.0) * 100.0, true);
            return;
        }
        if (g.pan.adjusted(-6.0, -8.0, 6.0, 8.0).contains(pos)) {
            drag_track_ = t;
            drag_volume_ = false;
            double const rel = (pos.x() - g.pan.left() - 4.0) /
                               (g.pan.width() - 8.0);
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
        double const rel = (pos.x() - g.vol.left() - 5.0) /
                           (g.vol.width() - 10.0);
        set_volume(drag_track_, std::clamp(rel, 0.0, 1.0) * 100.0, true);
    } else {
        double const rel = (pos.x() - g.pan.left() - 4.0) /
                           (g.pan.width() - 8.0);
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
    int const n = engine_.track_count();
    for (int t = 0; t < n; ++t) {
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

QRectF TrackStrips::add_bar_rect() const noexcept {
    double const h = add_bar_h();
    if (h <= 0.0) return {};
    return {8.0, height() - 8.0 - h, width() - 16.0, h};
}

void TrackStrips::set_volume(int track, double pct, bool report) {
    int const v = std::clamp(static_cast<int>(std::lround(pct)), 0, 100);
    volume_[static_cast<std::size_t>(track)] = v;
    // Engine is the truth: the strip's drag writes the per-track setter
    // (the TrackVolume command only reaches the SELECTED track).
    engine_.set_track_volume(track, static_cast<float>(v) / 100.0f);
    if (report && hook_) hook_(track, "volume", static_cast<double>(v));
    update();
}

void TrackStrips::set_pan(int track, double pan, bool report) {
    int const v = std::clamp(static_cast<int>(std::lround(pan)), -100, 100);
    pan_[static_cast<std::size_t>(track)] = v;
    // Engine pan space: 0 = hard left, 0.5 = center, 1 = hard right.
    engine_.set_track_pan(track,
                          static_cast<float>(v + 100) / 200.0f);
    if (report && hook_) hook_(track, "pan", static_cast<double>(v));
    update();
}

}  // namespace ghost_story::ui
