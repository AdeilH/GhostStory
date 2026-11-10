// ghost_story — src/ui/controller_mirror.cpp
#include "ui/controller_mirror.hpp"

#include "ui/theme.hpp"

#include <QComboBox>
#include <QWheelEvent>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QResizeEvent>
#include <QRadialGradient>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace ghost_story::ui {

namespace {

using theme::with_alpha;

constexpr int kKnobSweep = 270;          // degrees of knob travel
constexpr std::int64_t kPulseMs = 240;   // pad glow decay (ease-out)
constexpr std::int64_t kKnobGlowMs = 200;  // knob "just moved" glow
constexpr int kPadCount = 16;            // 2 banks x 8
constexpr double kPadGap = 4.0;          // pad cell gap (shared geometry)
constexpr double kKnobDragPx = 1.5;      // drag distance per 1 value step

// Per-pad hue tints. Bank A (drum kit family): kick warm red, snare cool
// steel, clap violet, hats brass, toms amber, perc teal, ride sage — each
// pad gets its own identity so hits are readable per voice. Bank B is the
// SAME 8 voices one octave up (pads semantics v2: all 16 pads drum in
// PLAY), so it wears the same hues as bank A — B4 reads as "A4, octave
// up", not as a different instrument.
constexpr int kPadHue[16] = {12,  205, 285, 55,  20,  35,  160, 95,
                             12,  205, 285, 55,  20,  35,  160, 95};

std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ControllerMirror::ControllerMirror(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    setToolTip(
        "Live mirror of the MPK Mini — and a playable one: the knobs and "
        "pads here ARE the controls.\n"
        "Pads — PLAY: all 16 fire the drum kit (A1-A8 the kit sounds, "
        "B1-B8 the same sounds one octave up). EDIT: A1-A8 select the "
        "track the keys play, B1/B2 undo/redo (also Ctrl+Z / Ctrl+Shift"
        "+Z), B3-B8 chord slots (hold a slot, play keys, release to "
        "capture — filled dot = slot holds a chord). KIT: every pad "
        "selects one of the 16 packs (also in the pack dropdown) and "
        "auditions its voice.\n"
        "Knobs — drag up/down, wheel to nudge, double-click to reset. The "
        "label under each names what it drives in the current mode (PLAY "
        "mixes the selected track, EDIT drives the FX panel, KIT tunes "
        "the kit), with the resolved CC number so remaps stay visible.\n"
        "Tracks — the keys ALWAYS play the SELECTED track (a Drums track "
        "maps keys onto the pads). Add or remove instruments from the "
        "track rail (+ / right-click).\n"
        "Keys chip — ONCE: a held key sounds exactly once (hardware "
        "arpeggiator re-fires are suppressed). REPEAT: every re-fire "
        "passes through.\n"
        "Click PLAY / EDIT / KIT, a track chip or a keys chip; pick a "
        "pack in the dropdown.");
    setMinimumHeight(252);
    // Minimum width the mirror's content actually needs: the 196px pad
    // well + a status column whose four track chips stay readable
    // (39px+ each). Without it the bottom row handed the mirror ~340px at
    // the window floor and the track chips elided down to empty squares —
    // 436 + FxPanel's 228 + HealthStrip's 224 + gaps is exactly the
    // window's 960 floor, so the promise "nothing clips at the floor"
    // holds by construction.
    setMinimumWidth(436);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Pack selector: a dropdown, not the old 16-chip grid (two wrapped
    // rows of 6pt text was the densest thing on the screen). Same engine
    // route the chips used — set_pad_pack on the UI thread, failures
    // logged, never swallowed. KIT-mode pads stay the fast performance
    // path; refresh() syncs this combo back from engine truth.
    pack_combo_ = new QComboBox(this);
    for (auto const& info : engine_.pad_packs()) {
        pack_combo_->addItem(QString::fromStdString(info.name));
        pack_combo_index_.push_back(info.index);
    }
    for (int i = 0; i < pack_combo_->count(); ++i) {
        if (pack_combo_index_[static_cast<std::size_t>(i)] ==
            engine_.current_pad_pack()) {
            pack_combo_->setCurrentIndex(i);  // before connect: no re-entry
            break;
        }
    }
    pack_combo_->setToolTip(
        "Pad pack — which 16 voices the pads drum. KIT mode maps all 16 "
        "packs onto the pads directly for mid-performance switching.");
    connect(pack_combo_, &QComboBox::currentIndexChanged, this,
            [this](int row) {
                if (row < 0 ||
                    row >= static_cast<int>(pack_combo_index_.size())) {
                    return;
                }
                std::string error;
                if (!engine_.set_pad_pack(
                        pack_combo_index_[static_cast<std::size_t>(row)],
                        error)) {
                    qWarning("pack combo row %d failed: %s", row,
                             error.c_str());
                }
                update();
            });
    pack_combo_->setStyleSheet(theme::combo_stylesheet());
    theme_generation_ = theme::generation();
}

void ControllerMirror::pulse_pad(int pad_index) {
    if (pad_index >= 0 && pad_index < kPadCount) {
        pad_hit_ms_[static_cast<std::size_t>(pad_index)] = steady_ms();
        update();
    }
}

void ControllerMirror::refresh() {
    // Theme switched? The combo's colors are baked into its stylesheet
    // string (not read at paint time like the painted widgets), so it
    // needs an explicit re-derive.
    if (theme::generation() != theme_generation_) {
        theme_generation_ = theme::generation();
        pack_combo_->setStyleSheet(theme::combo_stylesheet());
    }
    // Engine-truth sync: KIT-mode pads (and any other pack source) can
    // switch packs behind the combo's back. blockSignals so the sync does
    // not re-enter set_pad_pack (same pattern as HealthStrip's velocity
    // combo).
    int const current = engine_.current_pad_pack();
    for (int row = 0; row < pack_combo_->count(); ++row) {
        if (pack_combo_index_[static_cast<std::size_t>(row)] == current) {
            if (pack_combo_->currentIndex() != row) {
                pack_combo_->blockSignals(true);
                pack_combo_->setCurrentIndex(row);
                pack_combo_->blockSignals(false);
            }
            break;
        }
    }
    sync_knob_display();
    update();
}

void ControllerMirror::sync_knob_display() {
    // Engine-truth sync for the knobs with a clean inverse map: the PLAY
    // mix page (vol/pan/send of the SELECTED track — what the hardware
    // knobs drive) and the master fader (knob 8 in EDIT and KIT). Without
    // this the on-screen knobs sat at 0 while the engine ran at vol 100%,
    // and the first on-screen drag would have jumped the mix. FX and
    // instrument knobs keep their raw hardware positions (zone semantics,
    // no cheap inverse).
    auto const mode = engine_.mapping().mode();
    auto set = [this](int idx, double v01) {
        auto const v = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(v01 * 127.0)), 0, 127));
        engine_.mapping().set_knob_value(idx, v);
    };
    if (mode == mapping::Mode::Play) {
        int const t = engine_.selected_track();
        set(0, engine_.track_volume(t));
        set(1, engine_.track_pan(t));
        set(2, engine_.track_send(t));
    }
    set(7, static_cast<double>(engine_.master_gain()) / 1.5);
}

ControllerMirror::Geom ControllerMirror::compute_layout() const {
    // Layout: knobs strip on top, then one full-width row holding the pack
    // dropdown (a real child widget — resizeEvent places it from this same
    // math so the painted row and the widget can't drift), then pads
    // (left) and the mode/status column (right). WHY a dedicated row: pack
    // + track selection are the two global "where does input land"
    // choices, so they get full width instead of being squeezed into the
    // status column.
    double const pad = 8.0;
    QRectF const inner = QRectF(rect()).adjusted(pad, pad, -pad, -pad);
    // 0.42 of the inner height leaves room for the three text rows under
    // each knob (value / label / CC) without bleeding into the pack row.
    double const knobs_h = inner.height() * 0.42;
    double const rows_h = 26.0;  // one dropdown row (was 44: two chip rows)
    QRectF const knobs{inner.left(), inner.top(), inner.width(), knobs_h};
    double const rows_y = knobs.bottom() + 6.0;
    QRectF const packs{inner.left(), rows_y, inner.width(), rows_h};
    double const bottom_y = packs.bottom() + 6.0;
    double const pad_w = 4.0 * 44.0 + 3.0 * 4.0 + 8.0;  // 4x4 cells + gaps
    QRectF const pads{inner.left(), bottom_y, pad_w, inner.bottom() - bottom_y};
    QRectF const status{pads.right() + 12.0, bottom_y,
                        inner.right() - pads.right() - 12.0, pads.height()};
    return {knobs, packs, pads, status};
}

void ControllerMirror::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    now_ms_ = steady_ms();

    QRectF const full = QRectF(rect());

    // Panel: glass background, rounded corners, hairline border.
    theme::draw_panel(p, full);

    Geom const geom = compute_layout();

    theme::draw_well(p, geom.knobs, 8);
    theme::draw_well(p, geom.pads, 8);
    theme::draw_well(p, geom.status, 8);

    pads_rect_ = geom.pads;
    paint_knobs(p, geom.knobs);
    paint_pack_tag(p, geom.packs);
    paint_pads(p, geom.pads);
    paint_status(p, geom.status);
}

void ControllerMirror::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Place the pack dropdown inside its row using the same layout math
    // paintEvent reads (the +50 skips the painted "pack" tag column).
    if (pack_combo_ != nullptr) {
        QRectF const packs = compute_layout().packs;
        pack_combo_->setGeometry(
            static_cast<int>(packs.left() + 50.0),
            static_cast<int>(packs.top() + (packs.height() - 22.0) / 2.0),
            static_cast<int>(packs.width() - 50.0), 22);
    }
}

void ControllerMirror::paint_knobs(QPainter& p, QRectF const& area) {
    auto const mode = engine_.mapping().mode();

    QFont const value_font = theme::value_font(font(), 9);
    // Label + CC rows: the app's tiny-uppercase tier, not plain lowercase —
    // 8pt lowercase x-heights render ~4px tall and read as broken glyphs
    // (user report: "text being cut off" under every knob). Caps + tracking
    // at the same sizes stay legible, and the CC row keeps the resolved
    // number visible at reduced alpha.
    QFont const label_font = theme::label_font(font());
    QFontMetrics const value_fm{value_font};
    QFontMetrics const label_fm{label_font};
    double const row_v = static_cast<double>(value_fm.height()) + 2.0;
    double const row_l = static_cast<double>(label_fm.height()) + 2.0;
    double const text_h = row_v + 2.0 * row_l;

    double const cell_w = area.width() / 8.0;
    double const r = std::min(cell_w, area.height()) * 0.26;
    // Knob center: the preferred 32% band, clamped so the three text rows
    // under the knob (value / action / CC) always fit inside the strip.
    double const cy =
        std::min(area.top() + area.height() * 0.32,
                 area.bottom() - r - text_h);
    double const row_y = cy + r + 2.0;

    for (int i = 0; i < 8; ++i) {
        double const cx = area.left() + cell_w * (i + 0.5);
        QRectF const arc_box{cx - r, cy - r, 2 * r, 2 * r};
        // Hit target: the arc plus a small halo (fingers beat geometry).
        knob_rects_[static_cast<std::size_t>(i)] =
            arc_box.adjusted(-7.0, -7.0, 7.0, 7.0);

        int const v = engine_.mapping().knob_value(i);
        double const frac = static_cast<double>(v) / 127.0;

        // Knob body: recessed inner disc with a soft radial falloff so the
        // ring reads as a physical cap. Derived from the panel surface so
        // every theme gets its own micro-contrast (not hardcoded grays).
        QRadialGradient body{arc_box.center(), r};
        body.setColorAt(0.0, theme::shift(theme::pal().panel_top, 8));
        body.setColorAt(0.75, theme::shift(theme::pal().panel_top, -3));
        body.setColorAt(1.0, theme::shift(theme::pal().panel_bottom, -4));
        p.setPen(QPen(QColor(0, 0, 0, 120), 1.0));
        p.setBrush(body);
        p.drawEllipse(arc_box.center(), r * 0.62, r * 0.62);

        // Base ring: recessed 270° track starting at 225° (Qt angles: 1/16
        // deg, counter-clockwise positive).
        p.setPen(QPen(theme::shift(theme::pal().panel_top, 14), 3.5,
                      Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(arc_box, 225 * 16, -kKnobSweep * 16);

        // "Just moved" glow: wide translucent accent ring under the value
        // arc, fading over ~200 ms. WHY: at 30 Hz a single bright frame is
        // invisible; the short decay makes turns feel tactile.
        int const start16 = 225 * 16;
        int const sweep16 =
            -static_cast<int>(static_cast<double>(kKnobSweep) * frac) * 16;
        if (knob_last_[static_cast<std::size_t>(i)] != v) {
            knob_last_[static_cast<std::size_t>(i)] = v;
            knob_move_ms_[static_cast<std::size_t>(i)] = now_ms_;
        }
        double const age =
            static_cast<double>(now_ms_ -
                                knob_move_ms_[static_cast<std::size_t>(i)]) /
            static_cast<double>(kKnobGlowMs);
        if (frac > 0.0 && age < 1.0) {
            double const a = (1.0 - age) * 0.45;
            p.setPen(QPen(with_alpha(theme::pal().accent, a), 6.5, Qt::SolidLine,
                          Qt::RoundCap));
            p.drawArc(arc_box, start16, sweep16);
        }

        // Value arc: accent with a hot tip (short brighter cap at the end).
        if (frac > 0.0) {
            p.setPen(QPen(theme::pal().accent, 3.5, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_box, start16, sweep16);
            p.setPen(QPen(theme::pal().accent_hot, 3.5, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arc_box, start16 + sweep16, -6 * 16);
        }

        // Pointer line on the body, toward the value angle.
        double const angle_deg = 225.0 - kKnobSweep * frac;
        double const rad = angle_deg * 3.14159265358979323846 / 180.0;
        p.setPen(QPen(theme::pal().text, 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF{cx + r * 0.16 * std::cos(rad),
                           cy - r * 0.16 * std::sin(rad)},
                   QPointF{cx + r * 0.48 * std::cos(rad),
                           cy - r * 0.48 * std::sin(rad)});

        // Value, action label, resolved CC number — three calm rows under
        // the knob, each sized from its font's metrics (the old hardcoded
        // 13/11/10 boxes were shorter than the text they held).
        p.setPen(theme::pal().text);
        p.setFont(value_font);
        p.drawText(QRectF{cx - cell_w / 2, row_y, cell_w, row_v},
                   Qt::AlignHCenter | Qt::AlignTop, QString::number(v));
        p.setPen(theme::pal().muted);
        p.setFont(label_font);
        char const* lbl = mapping::MappingEngine::knob_label(mode, i);
        p.drawText(QRectF{cx - cell_w / 2, row_y + row_v, cell_w, row_l},
                   Qt::AlignHCenter | Qt::AlignTop,
                   lbl != nullptr ? QString::fromUtf8(lbl) : QString());
        p.setPen(with_alpha(theme::pal().muted, 0.75));
        p.drawText(QRectF{cx - cell_w / 2, row_y + row_v + row_l, cell_w, row_l},
                   Qt::AlignHCenter | Qt::AlignTop,
                   QString("cc %1").arg(engine_.mapping().knob_cc(i)));
    }
}

void ControllerMirror::paint_chip(QPainter& p, QRectF const& r,
                                  QString const& text, bool on,
                                  QColor const& hue, QFont const& chip_font,
                                  bool dot) {
    if (on) {
        QLinearGradient fill{r.topLeft(), r.bottomLeft()};
        fill.setColorAt(0.0, with_alpha(theme::pal().accent, 0.34));
        fill.setColorAt(1.0, with_alpha(theme::pal().accent, 0.16));
        p.setPen(QPen(theme::pal().accent, 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);
        p.setPen(theme::pal().text);
    } else {
        QLinearGradient fill{r.topLeft(), r.bottomLeft()};
        fill.setColorAt(0.0, theme::shift(theme::pal().panel_top, 5));
        fill.setColorAt(1.0, theme::shift(theme::pal().panel_top, -4));
        p.setPen(QPen(theme::pal().border, 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(r, 6, 6);
        p.setPen(theme::pal().muted);
    }
    p.setFont(chip_font);
    p.drawText(r, Qt::AlignCenter, text);

    // Hue dot (identity marker) on the left edge — quiet unless `on`.
    if (dot) {
        double const dot_r = 2.5;
        QColor const dot_c = on ? hue : with_alpha(hue, 0.55);
        p.setPen(Qt::NoPen);
        p.setBrush(dot_c);
        p.drawEllipse(QPointF{r.left() + 7.0, r.center().y()}, dot_r, dot_r);
    }
}

void ControllerMirror::paint_pack_tag(QPainter& p, QRectF const& area) {
    // Only the tag is painted here: the selector itself is the real
    // QComboBox child that resizeEvent places inside this row. The old
    // 16-chip grid wrapped to two rows of 6pt text and was the densest
    // element in the instrument — the dropdown reads calmer and still
    // reaches every pack (KIT pads remain the direct-access path).
    p.setPen(theme::pal().muted);
    p.setFont(theme::label_font(font()));
    p.drawText(QRectF{area.left(), area.top() + (area.height() - 18.0) / 2.0,
                      44.0, 18.0},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("pack"));
}

void ControllerMirror::paint_pads(QPainter& p, QRectF const& area) {
    QFont const pad_font{font().family(), std::max(font().pointSize() - 2, 6)};

    double const gap = kPadGap;
    double const cell_w = (area.width() - 3 * gap) / 4.0;
    double const cell_h = (area.height() - 3 * gap) / 4.0;

    for (int idx = 0; idx < kPadCount; ++idx) {
        // Columns 0-1: bank A pads A1..A8 (row-major); columns 2-3: B1..B8.
        int const local = idx % 8;
        int const col_base = idx < 8 ? 0 : 2;
        int const row = local / 2;
        int const col = col_base + local % 2;

        QRectF const cell{area.left() + col * (cell_w + gap),
                          area.top() + row * (cell_h + gap), cell_w, cell_h};

        // Every pad is live in every mode now (PLAY: 16 drums; EDIT:
        // select/undo/chords; KIT: pack grid) — no dimmed "other bank".
        int const hue = kPadHue[static_cast<std::size_t>(idx)];

        // Resting state: dark pad with the per-pad hue tint so voices stay
        // identifiable even before a hit. Light themes lift the resting
        // lightness so pads read as rubber, not holes punched in the panel.
        QColor fill = theme::is_light() ? QColor::fromHsl(hue, 95, 62)
                                        : QColor::fromHsl(hue, 100, 48);
        QColor border = theme::pal().accent;

        // Hit glow: ease-out decay over kPulseMs — a fast bright flash that
        // lingers just long enough to feel like a real rubber pad lighting
        // up (linear felt dead; the power curve keeps the tail visible).
        std::int64_t const hit = pad_hit_ms_[static_cast<std::size_t>(idx)];
        if (hit != 0) {
            double const age =
                static_cast<double>(now_ms_ - hit) / static_cast<double>(kPulseMs);
            if (age < 1.0) {
                double const a = std::pow(1.0 - age, 1.6);
                fill = QColor::fromHsl(hue, 190, 150);
                fill.setAlphaF(static_cast<float>(0.25 + 0.75 * a));
                // Soft halo while the glow is fresh.
                if (a > 0.2) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(with_alpha(QColor::fromHsl(hue, 190, 150),
                                          0.22 * a));
                    p.drawRoundedRect(cell.adjusted(-3.5, -3.5, 3.5, 3.5), 8, 8);
                }
                border = with_alpha(theme::pal().text, 0.4 + 0.6 * a);
            }
        }

        p.setPen(QPen(border, 1.4));
        p.setBrush(fill);
        p.drawRoundedRect(cell, 4, 4);
        // Top-edge inner highlight (pad "rubber" sheen).
        p.setPen(QPen(QColor(255, 255, 255, 14), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(cell.adjusted(1, 1, -1, -1), 3, 3);

        p.setPen(theme::pal().text);
        p.setFont(pad_font);
        QString const label =
            QString("%1%2").arg(idx < 8 ? QLatin1Char('A') : QLatin1Char('B'))
                .arg(local + 1);
        p.drawText(cell, Qt::AlignCenter, label);

        // Captured chord slots (B3..B8) wear a filled dot: an empty slot is
        // silent by design, and the dot separates "not captured yet" from
        // "pad broken" at a glance (user report: "BANK B doesn't work at
        // all" — empty slots gave no sound and no visible state).
        if (idx >= 10 && engine_.chord_slot_count(idx - 10) > 0) {
            QPointF const dot_c{cell.center().x(), cell.bottom() - 6.0};
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme::pal().text, 0.85));
            p.drawEllipse(dot_c, 2.6, 2.6);
        }
    }
}

void ControllerMirror::paint_status(QPainter& p, QRectF const& area) {
    auto const mode = engine_.mapping().mode();
    // The Step mode is the kit browser today (16 pads = 16 packs) — the
    // chip says so. If the pattern editor ever lands as a pad mode, this
    // label flips back to STEP.
    char const* mode_names[3] = {"PLAY", "EDIT", "KIT"};
    int const mode_idx = mode == mapping::Mode::Edit
                             ? 1
                             : (mode == mapping::Mode::Step ? 2 : 0);

    QFont const seg_font{font().family(), std::max(font().pointSize() - 1, 7)};
    QFont const chip_font{font().family(), std::max(font().pointSize() - 3, 6)};
    QFont const tiny{font().family(), std::max(font().pointSize() - 2, 6)};

    // Track chips (top of status column): the "where do keys land"
    // selector, one chip per ACTIVE track (dynamic pool — start empty).
    // Narrow panels: the hue dot drops out and the name elides — a name
    // cut mid-word reads as a bug, an ellipsis reads as a choice.
    double const tchip_h = 18.0;
    int const n_tracks = engine_.track_count();
    QFontMetrics const chip_fm{chip_font};
    // Tag column measured, not the old fixed 40px: "TRACK" tracked
    // uppercase is wider than 40px, so the K ran into the first chip's
    // hue dot. Both this row and the keys row share the widest tag so
    // their chips stay in one column.
    QFontMetrics const tag_fm{theme::label_font(font())};
    double const tag_w =
        8.0 + std::max(tag_fm.horizontalAdvance(QStringLiteral("track")),
                       tag_fm.horizontalAdvance(QStringLiteral("keys")));
    p.setFont(theme::label_font(font()));
    p.setPen(theme::pal().muted);
    p.drawText(QRectF{area.left(), area.top(), tag_w, tchip_h},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("track"));
    double const tw = n_tracks > 0
                          ? (area.width() - tag_w -
                             static_cast<double>(n_tracks - 1) * 5.0) /
                                static_cast<double>(n_tracks)
                          : 0.0;
    for (int i = 0; i < n_tracks; ++i) {
        QRectF const chip{area.left() + tag_w +
                              static_cast<double>(i) * (tw + 5.0),
                          area.top(), tw, tchip_h};
        bool const on = engine_.selected_track() == i;
        QColor const hue = theme::track_color(i);
        bool const show_dot = tw >= 40.0;  // 40px: dot + a 4-char elide fit
        QString const label = chip_fm.elidedText(
            QString::fromUtf8(engine_.track_name(i)), Qt::ElideRight,
            static_cast<int>(chip.width() - (show_dot ? 16.0 : 8.0)));
        paint_chip(p, chip, label, on, hue, chip_font, show_dot);
        track_rects_[static_cast<std::size_t>(i)] = chip;
    }
    if (n_tracks == 0) {
        p.setFont(tiny);
        p.setPen(with_alpha(theme::pal().muted, 0.85));
        p.drawText(QRectF{area.left() + tag_w + 6.0, area.top(),
                          area.width() - tag_w - 6.0, tchip_h},
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("add an instrument →"));
    }

    // Mode strip: sleek segmented control — recessed container, the active
    // segment filled with the accent gradient and a soft outer glow.
    double const seg_h = 26.0;
    double const seg_gap = 3.0;
    double const seg_w = (area.width() - 2 * seg_gap) / 3.0;

    QRectF const container{area.left(), area.top() + tchip_h + 5.0,
                           area.width(), seg_h};
    theme::draw_well(p, container, 7);

    for (int i = 0; i < 3; ++i) {
        QRectF const seg{container.left() + i * (seg_w + seg_gap) + 1.5,
                         container.top() + 1.5, seg_w - 3.0, seg_h - 3.0};
        mode_rects_[i] = seg;
        bool const on = i == mode_idx;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(with_alpha(theme::pal().accent, 0.14));
            p.drawRoundedRect(seg.adjusted(-2.0, -2.0, 2.0, 2.0), 7, 7);
            QLinearGradient fill{seg.topLeft(), seg.bottomLeft()};
            fill.setColorAt(0.0, theme::pal().accent_hot);
            fill.setColorAt(1.0, theme::pal().accent);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(seg, 5, 5);
            p.setPen(theme::is_light() ? theme::pal().panel_top
                                       : QColor(0x0b, 0x12, 0x1b));
        } else {
            p.setPen(theme::pal().muted);
        }
        p.setFont(seg_font);
        p.drawText(seg, Qt::AlignCenter, QString(mode_names[i]));
    }

    // Keys-response selector: what one physical key press does. "once" (the
    // default) sounds each key exactly once per hold — a hardware
    // arpeggiator re-firing a held key is suppressed; "repeat" passes every
    // re-fire through (arp/latch players want this). Radio chips, not a
    // bare toggle, so the state is always readable. Chips start at the
    // same tag_w column as the track chips above (see the tag measurement).
    double const keys_h = 18.0;
    p.setFont(theme::label_font(font()));
    p.setPen(theme::pal().muted);
    p.drawText(QRectF{area.left(), container.bottom() + 5.0, tag_w, keys_h},
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("keys"));
    double const kw = (area.width() - tag_w - 5.0) / 2.0;
    bool const once = engine_.keys_play_once();
    char const* keys_names[2] = {"once", "repeat"};
    for (int i = 0; i < 2; ++i) {
        QRectF const chip{area.left() + tag_w +
                              static_cast<double>(i) * (kw + 5.0),
                          container.bottom() + 5.0, kw, keys_h};
        bool const on = (i == 0) == once;
        paint_chip(p, chip, QString(keys_names[i]), on,
                   on ? theme::pal().accent
                       : theme::shift(theme::pal().panel_top, 14), chip_font);
        keys_rects_[static_cast<std::size_t>(i)] = chip;
    }

    // Pad legend (one calm line, elided — the full per-mode map lives in
    // the widget tooltip; the old 3-line word wrap was half the panel's
    // visual noise) + octave readout. The OCT box is sized to its text
    // and the legend elides into the remainder — the old fixed 70/28
    // split let the two collide at narrow widths.
    double const info_y = container.bottom() + 5.0 + keys_h + 5.0;
    int const oct = engine_.octave();
    QString const oct_text = QString("OCT %1%2")
                                 .arg(oct >= 0 ? QLatin1Char('+') : QLatin1Char('-'))
                                 .arg(oct < 0 ? -oct : oct);
    QFont const oct_font = theme::value_font(font(), 9);
    QFontMetrics const oct_fm{oct_font};
    double const oct_w = oct_fm.horizontalAdvance(oct_text) + 8.0;
    // Box heights from the fonts: drawText clips to the rect, and the old
    // hardcoded 16px boxes grazed the 'y' descenders of the legend on
    // font resolutions whose baseline landed lower.
    double const info_h =
        static_cast<double>(QFontMetrics{tiny}.height()) + 2.0;
    QRectF const legend{area.left(), info_y,
                        std::max(0.0, area.width() - oct_w - 10.0), info_h};
    p.setFont(tiny);
    p.setPen(theme::pal().muted);
    QFontMetrics const tfm{tiny};
    p.drawText(legend,
               Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine,
               tfm.elidedText(
                   QString("PLAY mixes · EDIT: A1-A8 track, knobs FX · "
                           "B1/B2 undo · B3-B8 chords · KIT: pads = packs"),
                   Qt::ElideRight, static_cast<int>(legend.width())));
    p.setPen(theme::pal().green);
    p.setFont(oct_font);
    p.drawText(QRectF{area.right() - oct_w, info_y, oct_w, info_h},
               Qt::AlignRight | Qt::AlignTop, oct_text);
}

// --- geometry + hit testing -------------------------------------------------

QRectF ControllerMirror::pad_cell(int idx) const noexcept {
    if (idx < 0 || idx >= kPadCount || pads_rect_.isEmpty()) return {};
    double const gap = kPadGap;
    double const cell_w = (pads_rect_.width() - 3 * gap) / 4.0;
    double const cell_h = (pads_rect_.height() - 3 * gap) / 4.0;
    int const local = idx % 8;
    int const col_base = idx < 8 ? 0 : 2;
    int const row = local / 2;
    int const col = col_base + local % 2;
    return {pads_rect_.left() + col * (cell_w + gap),
            pads_rect_.top() + row * (cell_h + gap), cell_w, cell_h};
}

int ControllerMirror::pad_at(QPointF const& pos) const noexcept {
    for (int i = 0; i < kPadCount; ++i) {
        if (pad_cell(i).contains(pos)) return i;
    }
    return -1;
}

// --- pad presses per mode (the same commands map_note emits) ----------------

void ControllerMirror::press_pad(int idx, bool pressed) {
    using A = mapping::Action;
    auto const mode = engine_.mapping().mode();
    if (mode == mapping::Mode::Play) {
        if (pressed) {
            engine_.command({A::DrumPad, idx, 0});
            pulse_pad(idx);  // UI commands skip drain_app_events — pulse here
        }
        return;
    }
    if (mode == mapping::Mode::Edit) {
        if (idx < 8) {  // bank A: track select
            if (pressed) {
                engine_.command({A::TrackSelect, idx, 0});
                pulse_pad(idx);
            }
            return;
        }
        if (idx == 8 || idx == 9) {  // B1/B2 undo/redo
            if (pressed) {
                engine_.command({idx == 8 ? A::Undo : A::Redo, 0, 0});
                pulse_pad(idx);
            }
            return;
        }
        // B3-B8 chord slots: press/release pairs (mouse-down = press, the
        // matching mouse-up = release — the hold gesture mirrors hardware).
        int const slot = idx - 10;
        engine_.command({A::ChordSlot, slot, pressed ? 0 : 1});
        if (pressed) {
            pulse_pad(idx);
            held_chord_pad_ = slot;
        }
        return;
    }
    // KIT: the pack browser (16 packs on 16 pads).
    if (pressed) {
        engine_.command({A::PadPackSelect, idx, 0});
        pulse_pad(idx);
    }
}

// --- mouse/wheel ------------------------------------------------------------

void ControllerMirror::mousePressEvent(QMouseEvent* event) {
    QPointF const pos = event->position();

    // Knobs: FxToggle (EDIT knob 2) is a click-to-toggle, not a drag; the
    // rest start a vertical drag (value 0..127, ±1 per kKnobDragPx).
    for (int i = 0; i < 8; ++i) {
        if (knob_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            auto const mode = engine_.mapping().mode();
            if (mapping::MappingEngine::knob_action(mode, i) ==
                mapping::Action::FxToggle) {
                engine_.command({mapping::Action::FxToggle, 0, 0});
                knob_move_ms_[static_cast<std::size_t>(i)] = steady_ms();
                update();
                return;
            }
            drag_knob_ = i;
            drag_knob_y0_ = pos.y();
            drag_knob_v0_ = engine_.mapping().knob_value(i);
            return;
        }
    }
    // Pads (before the chip rows — no overlap, but the pads read first).
    int const pad = pad_at(pos);
    if (pad >= 0) {
        press_pad(pad, true);
        update();
        return;
    }

    // Pack selection is the QComboBox child now (it handles its own
    // events before the parent sees them).
    // Track chips → select the track (keys/pads follow it).
    for (int i = 0; i < engine_.track_count(); ++i) {
        if (track_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            engine_.select_track(i);
            update();
            return;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (mode_rects_[i].contains(pos)) {
            auto const m = static_cast<mapping::Mode>(i);  // 0=Play 1=Edit 2=Kit
            engine_.set_mode(m);  // not mapping().set_mode — mode_rt_ (the
                                  // MIDI-thread pad gate) must follow
            update();
            return;
        }
    }
    // Keys-response chips: 0 = "once", 1 = "repeat".
    for (int i = 0; i < 2; ++i) {
        if (keys_rects_[i].contains(pos)) {
            engine_.set_keys_play_once(i == 0);
            update();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void ControllerMirror::mouseMoveEvent(QMouseEvent* event) {
    if (drag_knob_ < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    double const dy = drag_knob_y0_ - event->position().y();
    int const v = std::clamp(
        drag_knob_v0_ + static_cast<int>(std::lround(dy / kKnobDragPx)), 0,
        127);
    auto const mode = engine_.mapping().mode();
    auto const idx = static_cast<std::size_t>(drag_knob_);
    if (engine_.mapping().knob_value(drag_knob_) != v) {
        engine_.mapping().set_knob_value(drag_knob_,
                                         static_cast<std::uint8_t>(v));
        engine_.command({mapping::MappingEngine::knob_action(mode,
                                                             drag_knob_),
                         v, 0});
        knob_move_ms_[idx] = steady_ms();
        update();
    }
}

void ControllerMirror::mouseReleaseEvent(QMouseEvent* event) {
    if (drag_knob_ >= 0) {
        drag_knob_ = -1;
        update();
        return;
    }
    if (held_chord_pad_ >= 0) {
        press_pad(held_chord_pad_ + 10, false);  // release the chord slot
        held_chord_pad_ = -1;
        update();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ControllerMirror::mouseDoubleClickEvent(QMouseEvent* event) {
    QPointF const pos = event->position();
    for (int i = 0; i < 8; ++i) {
        if (knob_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            // Reset to the knob's neutral value (64) — volume/pan center,
            // FX param midpoint, etc. Same command path as a drag.
            engine_.mapping().set_knob_value(i, 64);
            engine_.command(
                {mapping::MappingEngine::knob_action(engine_.mapping().mode(),
                                                     i),
                 64, 0});
            knob_move_ms_[static_cast<std::size_t>(i)] = steady_ms();
            update();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void ControllerMirror::wheelEvent(QWheelEvent* event) {
    QPointF const pos = event->position();
    for (int i = 0; i < 8; ++i) {
        if (knob_rects_[static_cast<std::size_t>(i)].contains(pos)) {
            int const step = event->angleDelta().y() > 0 ? 1 : -1;
            int const v = std::clamp(engine_.mapping().knob_value(i) + step,
                                     0, 127);
            engine_.mapping().set_knob_value(i, static_cast<std::uint8_t>(v));
            engine_.command(
                {mapping::MappingEngine::knob_action(engine_.mapping().mode(),
                                                     i),
                 v, 0});
            knob_move_ms_[static_cast<std::size_t>(i)] = steady_ms();
            update();
            return;
        }
    }
    QWidget::wheelEvent(event);
}

}  // namespace ghost_story::ui
