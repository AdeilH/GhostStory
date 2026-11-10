// ghost_story — src/ui/fx_panel.cpp
#include "ui/fx_panel.hpp"

#include "ui/theme.hpp"

#include <QComboBox>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>
#include <string>

namespace ghost_story::ui {
namespace {

using theme::with_alpha;

/// Which FX block is ON for a track (chip lit). Mirrors the enable flags;
/// EQ/drive have composite state (a band != 0 dB / drive > 1).
bool slot_on(app::PlayEngine& engine, int track, int slot) {
    auto const p = engine.track_fx_params(track);
    switch (static_cast<app::PlayEngine::FxSlot>(slot)) {
    case app::PlayEngine::FxSlot::Eq:
        return p.eq_low_db != 0.f || p.eq_mid_db != 0.f ||
               p.eq_high_db != 0.f;
    case app::PlayEngine::FxSlot::Comp: return p.comp_on;
    case app::PlayEngine::FxSlot::Crush: return p.crush_on;
    case app::PlayEngine::FxSlot::Drive: return p.drive > 1.0005f;
    case app::PlayEngine::FxSlot::Chorus: return p.chorus_on;
    case app::PlayEngine::FxSlot::Flanger: return p.flanger_on;
    case app::PlayEngine::FxSlot::Delay: return p.delay_on;
    case app::PlayEngine::FxSlot::Tremolo: return p.tremolo_on;
    case app::PlayEngine::FxSlot::Reverb: return p.reverb_on;
    }
    return false;
}

}  // namespace

FxPanel::FxPanel(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip(
        "Per-track FX chain + presets.\n"
        "Chips — the nine effects in signal order; click to select, "
        "click the dot corner (or right-click) to toggle the effect "
        "on/off. The lit dot = on.\n"
        "A/B lanes — the selected effect's two knobs; drag to set.\n"
        "Preset — curated synth-patch + FX-pair loads for the selected "
        "track; manual knob edits mark the track (custom). Drums uses pad "
        "packs instead, so the dropdown disables itself there.\n"
        "The bottom line explains the selected effect — or the last "
        "preset error, if one failed.");
    setMinimumHeight(150);
    // Narrowest width where the wrapping slot chips, the A/B lanes and
    // the preset combo all stay usable (the chips wrap to 3 rows here —
    // compute_layout reflows everything above them). Pairs with the
    // mirror's 436 minimum: together with the health strip they fill the
    // window's 960 floor exactly.
    setMinimumWidth(228);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Preset selector: a dropdown, not the old 10-chip grid (the chips
    // wrapped to a second row of 6pt text in the densest panel). Row 0 is
    // the "(custom)" placeholder so a manually edited track shows a state
    // instead of an empty box; engine preset r lives at combo row r+1.
    preset_combo_ = new QComboBox(this);
    repopulate_presets();
    preset_combo_->setToolTip(
        "Preset — loads a curated synth patch + FX chain onto the selected "
        "track. Manual knob edits switch it to (custom).");
    connect(preset_combo_, &QComboBox::currentIndexChanged, this,
            [this](int row) {
                int const track = engine_.selected_track();
                if (row <= 0 ||
                    engine_.track_kind(track) == app::TrackKind::Drums) {
                    return;  // row 0 is the placeholder, drums never apply
                }
                std::string err;
                if (!engine_.apply_preset(row - 1, track, err)) {
                    preset_error_ = QString::fromStdString(err);
                } else {
                    preset_error_.clear();
                }
                update();
            });
    preset_combo_->setStyleSheet(theme::combo_stylesheet());
    theme_generation_ = theme::generation();
}

void FxPanel::refresh() {
    // Theme switched? The combo's colors live in its stylesheet string,
    // so it needs an explicit re-derive (the painted rows just re-read
    // pal() on the next frame).
    if (theme::generation() != theme_generation_) {
        theme_generation_ = theme::generation();
        preset_combo_->setStyleSheet(theme::combo_stylesheet());
    }
    // Track switch: repopulate (drums get the disabled hint row). Same
    // track: resync the selection with engine truth — hardware presets,
    // failed applies, and manual knob edits (preset -> (custom)) all
    // change current_preset behind the combo's back. blockSignals so the
    // sync never re-enters apply_preset.
    int const track = engine_.selected_track();
    if (track != preset_combo_track_) {
        repopulate_presets();
    } else if (engine_.track_kind(track) != app::TrackKind::Drums) {
        int const cur = engine_.current_preset(track);
        int const want = cur >= 0 ? cur + 1 : 0;
        if (preset_combo_->currentIndex() != want) {
            preset_combo_->blockSignals(true);
            preset_combo_->setCurrentIndex(want);
            preset_combo_->blockSignals(false);
        }
    }
    update();
}

void FxPanel::repopulate_presets() {
    int const track = engine_.selected_track();
    preset_combo_->blockSignals(true);
    preset_combo_->clear();
    if (engine_.track_kind(track) == app::TrackKind::Drums) {
        // apply_preset is a documented no-op for the drums track ("packs
        // are the drum presets") — disable with a hint instead of letting
        // every pick paint a red error (the old chips did exactly that).
        preset_combo_->addItem(QStringLiteral("(drums: pads pick packs)"));
        preset_combo_->setEnabled(false);
    } else {
        preset_combo_->setEnabled(true);
        preset_combo_->addItem(QStringLiteral("(custom)"));
        for (auto const& p : engine_.presets()) {
            preset_combo_->addItem(QString::fromStdString(p.name));
        }
        int const cur = engine_.current_preset(track);
        preset_combo_->setCurrentIndex(cur >= 0 ? cur + 1 : 0);
    }
    preset_combo_track_ = track;
    preset_combo_->blockSignals(false);
}

FxPanel::Geom FxPanel::compute_layout() const {
    // Row stack (see paintEvent): slot chips (greedy-wrapped when narrow)
    // / A+B lanes / preset combo / description strip. The preset combo is
    // a real child widget placed by resizeEvent from this same math,
    // vertically centered in whatever room remains between the lanes and
    // the bottom strip.
    double const pad = 8.0;
    QRectF const inner = QRectF(rect()).adjusted(pad, pad, -pad, -pad);
    double const chip_h = 20.0;
    double const chip_gap = 4.0;
    double const lane_h = 16.0;
    double const combo_h = 24.0;
    // Description strip: one line of 9pt text + slack — drawText clips to
    // the rect, and a height equal to the line box grazed descenders.
    double const desc_h =
        static_cast<double>(QFontMetrics{theme::label_font(font())}.height()) + 2.0;

    // Slot chips: greedy wrap when the panel is too narrow for all nine
    // on one line — the old single row overflowed the panel edge and let
    // Delay/Trem/Verb clip at the window floor. Rects are computed HERE
    // so the band height, the paint pass and the hit rects agree by
    // construction.
    QFont const chip_font = theme::value_font(font(), 9);
    QFontMetrics const fm{chip_font};
    double const chips_x0 = inner.left() + 34.0;
    double x = chips_x0;
    double y = inner.top();
    int rows = 1;
    std::array<QRectF, app::PlayEngine::kFxSlotCount> rects{};
    for (int i = 0; i < app::PlayEngine::kFxSlotCount; ++i) {
        double const w =
            fm.horizontalAdvance(QString::fromLatin1(
                app::PlayEngine::fx_slot_name(i))) +
            10.0;
        if (x > chips_x0 && x + w > inner.right()) {
            ++rows;
            x = chips_x0;
            y += chip_h + chip_gap;
        }
        rects[static_cast<std::size_t>(i)] = {x, y, w, chip_h};
        x += w + chip_gap;
    }
    double const band_h = static_cast<double>(rows) * chip_h +
                          static_cast<double>(rows - 1) * chip_gap;

    double const y1 = inner.top() + band_h + 6.0;
    double const y2 = y1 + lane_h + 6.0;
    double const desc_y = inner.bottom() - desc_h;
    double const combo_y =
        y2 + std::max(0.0, (desc_y - 4.0 - y2 - combo_h) / 2.0);
    // Tag column measured, not fixed: "PRESET" tracked uppercase is wider
    // than the old 48px offset, so the tag ran under the combo. paintEvent
    // derives the tag box from presets.left() so the two can't drift.
    double const preset_tag_w =
        QFontMetrics{theme::label_font(font())}.horizontalAdvance(
            QStringLiteral("preset")) +
        10.0;
    return {rects,
            rows,
            {inner.left(), inner.top(), inner.width(), band_h},
            {inner.left(), y1, inner.width(), lane_h},
            {inner.left() + preset_tag_w, combo_y, inner.width() - preset_tag_w,
             combo_h},
            {inner.left(), desc_y, inner.width(), desc_h}};
}

void FxPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Place the preset dropdown inside its row using the same layout math
    // paintEvent reads (the +48 skips the painted "preset" tag column).
    if (preset_combo_ != nullptr) {
        QRectF const r = compute_layout().presets;
        preset_combo_->setGeometry(static_cast<int>(r.left()),
                                   static_cast<int>(r.top()),
                                   static_cast<int>(r.width()),
                                   static_cast<int>(r.height()));
    }
}

double FxPanel::param_value(int ab) const {
    // Normalized display value; mirrors the fx_knob parameter maps in
    // PlayEngine::command() (inverse of each mapping). Drums has FX too.
    int const track = engine_.selected_track();
    auto const p = engine_.track_fx_params(track);
    switch (static_cast<app::PlayEngine::FxSlot>(engine_.fx_slot())) {
    case app::PlayEngine::FxSlot::Eq:
        return ab == 0 ? (static_cast<double>(p.eq_low_db) + 18.0) / 36.0
                       : (static_cast<double>(p.eq_high_db) + 18.0) / 36.0;
    case app::PlayEngine::FxSlot::Comp:
        return ab == 0
                   ? (static_cast<double>(p.comp_threshold) - 0.02) / 0.98
                   : (static_cast<double>(p.comp_ratio) - 1.0) / 19.0;
    case app::PlayEngine::FxSlot::Crush:
        return ab == 0 ? (static_cast<double>(p.crush_bits) - 2.0) / 14.0
                       : (static_cast<double>(p.crush_downsample) - 1.0) /
                             15.0;
    case app::PlayEngine::FxSlot::Drive:
        if (ab == 0) {
            return (static_cast<double>(p.drive) - 1.0) / 7.0;
        }
        // Shape: three zones matching the knob thresholds.
        switch (p.drive_shape) {
        case audio::DriveShape::Soft: return 0.16;
        case audio::DriveShape::Hard: return 0.50;
        default: return 0.83;
        }
    case app::PlayEngine::FxSlot::Chorus:
        return ab == 0
                   ? (static_cast<double>(p.chorus_rate_hz) - 0.01) / 9.99
                   : static_cast<double>(p.chorus_mix);
    case app::PlayEngine::FxSlot::Flanger:
        return ab == 0
                   ? (static_cast<double>(p.flanger_rate_hz) - 0.05) / 9.95
                   : static_cast<double>(p.flanger_mix);
    case app::PlayEngine::FxSlot::Delay:
        if (ab == 0) {
            // Zone midpoints for the five note values.
            if (p.delay_beats <= 0.0626f) return 0.10;   // 1/16
            if (p.delay_beats <= 0.1251f) return 0.30;   // 1/8
            if (p.delay_beats <= 0.2501f) return 0.50;   // 1/4
            if (p.delay_beats <= 0.5001f) return 0.70;   // 1/2
            return 0.90;                                  // 1
        }
        return static_cast<double>(p.delay_mix);
    case app::PlayEngine::FxSlot::Tremolo:
        return ab == 0
                   ? (static_cast<double>(p.tremolo_rate_hz) - 0.05) / 19.95
                   : static_cast<double>(p.tremolo_depth);
    case app::PlayEngine::FxSlot::Reverb:
        return ab == 0 ? static_cast<double>(p.reverb_decay)
                       : static_cast<double>(p.reverb_mix);
    }
    return 0.0;
}

void FxPanel::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF const full = QRectF(rect());
    theme::draw_panel(p, full);

    double const pad = 8.0;
    QRectF const inner = full.adjusted(pad, pad, -pad, -pad);
    QFont const base = font();
    QFont const label = theme::label_font(base);
    QFont const chip_font = theme::value_font(base, 9);
    QFont const tiny = theme::label_font(base);
    QFontMetrics const tiny_fm{tiny};

    int const track = engine_.selected_track();
    int const slot = engine_.fx_slot();

    // --- row 1: FX slot chips (greedy-wrapped by compute_layout when the
    // panel is too narrow for one line) ----------------------------------
    Geom const geom = compute_layout();
    p.setFont(label);
    p.setPen(theme::pal().muted);
    p.drawText(QRectF{inner.left(), geom.chips.top(), 30.0,
                      geom.chips.height()},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("fx"));
    for (int i = 0; i < app::PlayEngine::kFxSlotCount; ++i) {
        char const* nm = app::PlayEngine::fx_slot_name(i);
        QRectF const& r = geom.chip_rects[static_cast<std::size_t>(i)];
        bool const sel = i == slot;
        bool const on = slot_on(engine_, track, i);
        // Selected: accent fill. Unselected but on: accent outline + dot —
        // the player sees the whole chain state at a glance.
        if (sel) {
            theme::draw_well(p, r, 6);
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0.0, with_alpha(theme::pal().accent, 0.85));
            g.setColorAt(1.0, with_alpha(theme::pal().accent, 0.55));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(r, 6, 6);
            p.setPen(theme::is_light() ? theme::pal().panel_top
                                       : QColor(0x0b, 0x12, 0x1b));
        } else {
            theme::draw_well(p, r, 6);
            p.setPen(QPen(with_alpha(theme::pal().accent, on ? 0.9 : 0.25), 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            p.setPen(on ? theme::pal().text : theme::pal().muted);
        }
        p.setFont(chip_font);
        p.drawText(r, Qt::AlignCenter, QString::fromUtf8(nm));
        // ON dot (top-right corner).
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme::pal().accent);
            p.drawEllipse(QPointF{r.right() - 5.0, r.top() + 5.0}, 2.2, 2.2);
        }
        slot_rects_[static_cast<std::size_t>(i)] = r;
    }

    // --- row 2: param A/B lanes -----------------------------------------
    double const lane_h = geom.lanes.height();
    // Label column measured against the widest of the two names (tracked
    // uppercase ranges from "A RATE" to "B DOWNSAMPLE" — the old fixed 70px
    // let the long ones run under the lane well), capped so the lane keeps
    // usable room at the panel's narrow floor. The drawn text elides into
    // the same column, so label and lane can never overlap.
    char const* pa = app::PlayEngine::fx_slot_param_name(slot, 0);
    char const* pb = app::PlayEngine::fx_slot_param_name(slot, 1);
    double const half = (inner.width() - 8.0) / 2.0;
    double const label_w = std::min(
        8.0 + std::max(
                  tiny_fm.horizontalAdvance(QString("A %1").arg(
                      QString::fromUtf8(pa))),
                  tiny_fm.horizontalAdvance(QString("B %1").arg(
                      QString::fromUtf8(pb)))),
        std::max(56.0, half - 44.0));
    for (int ab = 0; ab < 2; ++ab) {
        double const lx = inner.left() +
                          static_cast<double>(ab) * (half + 8.0);
        QRectF const lane{lx + label_w, geom.lanes.top(),
                          std::max(half - label_w - 6.0, 24.0), lane_h};
        p.setFont(tiny);
        p.setPen(theme::pal().muted);
        p.drawText(QRectF{lx, geom.lanes.top(), label_w, lane_h},
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                   tiny_fm.elidedText(
                       QString("%1 %2")
                           .arg(ab == 0 ? "A" : "B",
                                QString::fromUtf8(ab == 0 ? pa : pb)),
                       Qt::ElideRight, static_cast<int>(label_w)));
        theme::draw_well(p, lane, 5);
        double const v =
            std::clamp(param_value(ab), 0.0, 1.0);
        if (v > 0.0) {
            QRectF fill{lane.left() + 1, lane.top() + 1,
                        (lane.width() - 2) * v, lane.height() - 2};
            QLinearGradient g(fill.topLeft(), fill.bottomLeft());
            g.setColorAt(0.0, with_alpha(theme::pal().accent, 0.9));
            g.setColorAt(1.0, with_alpha(theme::pal().accent, 0.6));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawRoundedRect(fill, 4, 4);
        }
        // Handle dot at the value position.
        p.setPen(Qt::NoPen);
        p.setBrush(theme::pal().text);
        p.drawEllipse(QPointF{lane.left() + 1 + (lane.width() - 2) * v,
                              lane.center().y()},
                      3.2, 3.2);
        param_rects_[static_cast<std::size_t>(ab)] = lane;
    }

    // --- row 3: preset dropdown — only the tag is painted here; the
    // selector itself is the real QComboBox child placed by resizeEvent
    // inside geom.presets (the old 10-chip grid wrapped to a second row
    // of 6pt text and was the densest element in this panel) -------------
    p.setFont(tiny);
    p.setPen(theme::pal().muted);
    // The tag box is derived from the combo's left edge (the layout truth
    // computed in compute_layout) minus the gap — it can't under- or
    // overflow the column no matter what the font metrics say.
    p.drawText(QRectF{inner.left(), geom.presets.top(),
                      geom.presets.left() - inner.left() - 10.0,
                      geom.presets.height()},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("preset"));

    // --- bottom strip: what the selected effect does (or the last preset
    // error, so a failed pick explains itself instead of failing silent) --
    // Elided, not clipped: a run-on sentence chopped at the panel edge
    // reads as a bug, an ellipsis reads as a choice.
    p.setFont(tiny);
    if (!preset_error_.isEmpty()) {
        p.setPen(theme::pal().record);
        p.drawText(geom.desc, Qt::AlignLeft | Qt::AlignVCenter |
                                  Qt::TextSingleLine,
                   tiny_fm.elidedText(preset_error_, Qt::ElideRight,
                                      static_cast<int>(geom.desc.width())));
    } else if (char const* d = app::PlayEngine::fx_slot_desc(slot)) {
        p.setPen(theme::pal().muted);
        p.drawText(geom.desc, Qt::AlignLeft | Qt::AlignVCenter |
                                  Qt::TextSingleLine,
                   tiny_fm.elidedText(QString::fromUtf8(d), Qt::ElideRight,
                                      static_cast<int>(geom.desc.width())));
    }
}

void FxPanel::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();
    // Slot chips: select the slot; a click on the ON-dot corner (or a
    // right-click anywhere on a chip) TOGGLES the effect — the same
    // FxToggle action EDIT-mode hardware knob 2 emits. The tooltip always
    // promised this; now the dot actually answers.
    for (int i = 0; i < app::PlayEngine::kFxSlotCount; ++i) {
        if (slot_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            QRectF const& r = slot_rects_[static_cast<std::size_t>(i)];
            QRectF const dot{r.right() - 12.0, r.top(), 12.0, 12.0};
            bool const toggle =
                dot.contains(pos) ||
                event->button() == Qt::MouseButton::RightButton;
            engine_.set_fx_slot(i);
            if (toggle) {
                engine_.command({mapping::Action::FxToggle, 0, 0});
            }
            update();
            return;
        }
    }
    // Param lanes: start a drag (absolute-from-origin).
    for (int ab = 0; ab < 2; ++ab) {
        QRectF const& lane = param_rects_[static_cast<std::size_t>(ab)];
        if (lane.adjusted(-6, -6, 6, 6).contains(pos)) {
            dragging_ = ab;
            drag_origin_x_ = pos.x();
            drag_origin_value_ =
                std::clamp(param_value(ab), 0.0, 1.0);
            return;
        }
    }
    // Preset selection is the QComboBox child now (it handles its own
    // events before the parent sees them).
    QWidget::mousePressEvent(event);
}

void FxPanel::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_ < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // Drag = absolute position across the lane (not a delta from the
    // origin) — direct manipulation, matching the transport-bar click
    // slider. Snapping to the lane bounds is implicit via the 0..127 map.
    QRectF const& lane = param_rects_[static_cast<std::size_t>(dragging_)];
    double const t =
        std::clamp((event->position().x() - lane.left()) /
                       std::max(lane.width(), 1.0),
                   0.0, 1.0);
    int const param = static_cast<int>(std::lround(t * 127.0));
    mapping::Command c;
    c.action = dragging_ == 0 ? mapping::Action::FxParamA
                              : mapping::Action::FxParamB;
    c.param = param;
    engine_.command(c);
    update();
}

void FxPanel::mouseReleaseEvent(QMouseEvent* event) {
    dragging_ = -1;
    QWidget::mouseReleaseEvent(event);
}

}  // namespace ghost_story::ui
