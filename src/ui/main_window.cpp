// addu94 — src/ui/main_window.cpp
#include "ui/main_window.hpp"

#include "log/logger.hpp"
#include "ui/theme.hpp"

#include <QCursor>
#include <QFont>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QShortcut>
#include <QTimerEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace addu94::ui {

namespace {

using theme::kAccent;
using theme::kBorder;
using theme::kMuted;
using theme::kRecord;
using theme::kText;
using theme::with_alpha;

constexpr int kUiHz = 30;                 // app-event drain / repaint rate
constexpr int kUiTimerMs = 1000 / kUiHz;  // 33 ms
constexpr double kPpq = 96.0;             // ticks per quarter (music/transport.hpp)
constexpr double kBarHeight = 64.0;       // transport bar height

// Transport buttons are drawn shapes (no text glyphs), 36px.
constexpr double kBtnSize = 36.0;
constexpr double kBtnGap = 8.0;

std::int64_t steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ===========================================================================
// TransportBar — custom-painted transport row (drawn icons, drag values).
// ===========================================================================
class TransportBar : public QWidget {
public:
    explicit TransportBar(app::PlayEngine& engine, QWidget* parent)
        : QWidget(parent), engine_(engine) {
        setFixedHeight(static_cast<int>(kBarHeight));
        setMouseTracking(true);
        setToolTip(
            "Transport — play (space), stop, record (R), metronome (M). "
            "Drag BPM/LOOP vertically to change, double-click to type. "
            "Drag the click bar for metronome level");
    }

    // Shortcut entries (MainWindow wires Space / R / M).
    void shortcut_play() {
        bool const playing = position_advancing();
        engine_.audio().transport_control().state_req.store(
            playing ? music::TransportControl::kReqStop
                    : music::TransportControl::kReqPlay,
            std::memory_order_release);
    }
    void shortcut_record() { engine_.command({mapping::Action::TransportRecord, 0, 0}); }
    void shortcut_metronome() {
        auto& ctrl = engine_.audio().transport_control();
        ctrl.metronome_on.store(!ctrl.metronome_on.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
    }

    /// UI-tick: update the playing flag from transport position deltas.
    void ui_tick() {
        playing_ = position_advancing();
        update();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        now_ms_ = steady_ms();

        QRectF const full = QRectF(rect());
        theme::draw_panel(p, full);

        QFont const value_font{font().family(), 13, QFont::DemiBold};
        QFont const label_font{font().family(), 8};
        QFont const hint_font{font().family(), 9};

        // --- buttons: play / stop / record / metronome ---
        double const by = (full.height() - kBtnSize - 14.0) / 2.0 + 2.0;
        double const bx0 = 14.0;
        for (int i = 0; i < 4; ++i) {
            QRectF const btn{bx0 + static_cast<double>(i) * (kBtnSize + kBtnGap),
                             by, kBtnSize, kBtnSize};
            btn_rects_[i] = btn;
            paint_button(p, btn, i);
            // Tiny keyboard hints under the buttons.
            p.setPen(kMuted);
            p.setFont(hint_font);
            char const* hint = i == 0 ? "space"
                               : i == 2 ? "R"
                               : i == 3 ? "M"
                                        : "";
            if (hint[0] != '\0') {
                p.drawText(QRectF{btn.left() - 6.0, btn.bottom() + 1.0,
                                  btn.width() + 12.0, 12.0},
                           Qt::AlignCenter, QString(hint));
            }
        }

        double x = btn_rects_[3].right() + 14.0;

        // --- BPM (drag vertically, double-click to type) ---
        x = paint_value_block(p, x, "BPM", bpm_text(), value_font, label_font,
                              hover_ == Hover::Bpm, "+/- drag");
        bpm_rect_ = QRectF{x - 74.0, 8.0, 74.0, full.height() - 16.0};

        // --- meter (read-only from the control plane) ---
        x = paint_value_block(p, x, "METER", meter_text(), value_font,
                              label_font, false, "");
        meter_rect_ = QRectF{x - 58.0, 8.0, 58.0, full.height() - 16.0};

        // --- loop bars (drag; 0 = ∞) ---
        x = paint_value_block(p, x, "LOOP", loop_text(), value_font,
                              label_font, hover_ == Hover::Loop, "+/- drag");
        loop_rect_ = QRectF{x - 66.0, 8.0, 66.0, full.height() - 16.0};

        // --- click (metronome gain): thin slider ---
        p.setPen(kMuted);
        p.setFont(label_font);
        p.drawText(QRectF{x, full.height() / 2.0 - 14.0, 64.0, 11.0},
                   Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("click"));
        QRectF const track{x, full.height() / 2.0 + 1.0, 64.0, 4.0};
        click_rect_ = track.adjusted(-8.0, -12.0, 8.0, 12.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x27, 0x2c, 0x35));
        p.drawRoundedRect(track, 2, 2);
        float const gain =
            engine_.audio().transport_control().metronome_gain.load(
                std::memory_order_relaxed);
        double const fill = track.width() * std::clamp(static_cast<double>(gain), 0.0, 1.0);
        p.setBrush(kAccent);
        p.drawRoundedRect(QRectF{track.left(), track.top(), fill, track.height()},
                          2, 2);
        p.setBrush(kText);
        p.drawEllipse(QPointF{track.left() + fill, track.center().y()}, 5.0, 5.0);

        // --- position readout (right) ---
        QFont mono{"monospace"};
        mono.setStyleHint(QFont::TypeWriter);
        mono.setPointSize(std::max(font().pointSize() + 1, 10));
        p.setFont(mono);
        p.setPen(kText);
        p.drawText(QRectF{full.right() - 220.0, 0.0, 206.0,
                          static_cast<double>(height())},
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::fromStdString(position_text()));
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        QPointF const pos = event->position();
        Hover const prev = hover_;
        hover_ = bpm_rect_.contains(pos)     ? Hover::Bpm
                 : loop_rect_.contains(pos)  ? Hover::Loop
                                             : Hover::None;
        if (drag_ == Drag::Bpm) {
            auto& ctrl = engine_.audio().transport_control();
            double const v = drag_bpm0_ + (drag_y0_ - pos.y()) / 4.0;  // ±1 per 4px
            ctrl.tempo_bpm.store(std::clamp(v, 20.0, 333.0),
                                 std::memory_order_relaxed);
        } else if (drag_ == Drag::Loop) {
            auto& ctrl = engine_.audio().transport_control();
            int const v = drag_loop0_ +
                          static_cast<int>(std::lround((drag_y0_ - pos.y()) / 12.0));
            ctrl.loop_bars.store(static_cast<std::uint64_t>(std::clamp(v, 0, 16)),
                                 std::memory_order_relaxed);
        } else if (drag_ == Drag::Click) {
            auto& ctrl = engine_.audio().transport_control();
            double const rel = (pos.x() - click_rect_.left()) / click_rect_.width();
            ctrl.metronome_gain.store(
                static_cast<float>(std::clamp(rel, 0.0, 1.0)),
                std::memory_order_relaxed);
        }
        if (hover_ != prev || drag_ != Drag::None) update();
    }

    void mousePressEvent(QMouseEvent* event) override {
        QPointF const pos = event->position();
        auto& ctrl = engine_.audio().transport_control();

        for (int i = 0; i < 4; ++i) {
            if (btn_rects_[i].contains(pos)) {
                press_button(i);
                return;
            }
        }
        if (bpm_rect_.contains(pos)) {
            drag_ = Drag::Bpm;
            drag_y0_ = pos.y();
            drag_bpm0_ = ctrl.tempo_bpm.load(std::memory_order_relaxed);
            return;
        }
        if (loop_rect_.contains(pos)) {
            drag_ = Drag::Loop;
            drag_y0_ = pos.y();
            drag_loop0_ = static_cast<int>(
                ctrl.loop_bars.load(std::memory_order_relaxed));
            return;
        }
        if (click_rect_.contains(pos)) {
            drag_ = Drag::Click;
            double const rel = (pos.x() - click_rect_.left()) / click_rect_.width();
            ctrl.metronome_gain.store(
                static_cast<float>(std::clamp(rel, 0.0, 1.0)),
                std::memory_order_relaxed);
            update();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        drag_ = Drag::None;
        update();
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        QPointF const pos = event->position();
        auto& ctrl = engine_.audio().transport_control();
        if (bpm_rect_.contains(pos)) {
            bool ok = false;
            double const v = QInputDialog::getDouble(
                this, QStringLiteral("Tempo"), QStringLiteral("BPM (20..333):"),
                ctrl.tempo_bpm.load(std::memory_order_relaxed), 20.0, 333.0, 1,
                &ok);
            if (ok) {
                ctrl.tempo_bpm.store(v, std::memory_order_relaxed);
            }
            update();
            return;
        }
        if (loop_rect_.contains(pos)) {
            bool ok = false;
            int const v = QInputDialog::getInt(
                this, QStringLiteral("Loop"),
                QStringLiteral("Loop length in bars (0 = off):"),
                static_cast<int>(ctrl.loop_bars.load(std::memory_order_relaxed)),
                0, 16, 1, &ok);
            if (ok) {
                ctrl.loop_bars.store(static_cast<std::uint64_t>(v),
                                     std::memory_order_relaxed);
            }
            update();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hover_ = Hover::None;
        update();
        QWidget::leaveEvent(event);
    }

private:
    enum class Hover : std::uint8_t { None, Bpm, Loop };
    enum class Drag : std::uint8_t { None, Bpm, Loop, Click };

    [[nodiscard]] bool position_advancing() {
        std::uint64_t const pos = engine_.audio().transport_position();
        bool const advancing = pos != last_pos_;
        last_pos_ = pos;
        return advancing;
    }

    [[nodiscard]] QString bpm_text() const {
        double const bpm = engine_.audio().transport_control().tempo_bpm.load(
            std::memory_order_relaxed);
        return QString::fromStdString(std::format("{:.1f}", bpm));
    }

    [[nodiscard]] QString meter_text() const {
        auto const& ctrl = engine_.audio().transport_control();
        return QString("%1/%2")
            .arg(ctrl.meter_num.load(std::memory_order_relaxed))
            .arg(ctrl.meter_den.load(std::memory_order_relaxed));
    }

    [[nodiscard]] QString loop_text() const {
        std::uint64_t const v = engine_.audio().transport_control().loop_bars.load(
            std::memory_order_relaxed);
        return v == 0 ? QStringLiteral("\u221E") : QString::number(
                                                         static_cast<long long>(v));
    }

    [[nodiscard]] std::string position_text() const {
        auto const& ctrl = engine_.audio().transport_control();
        auto const stats = engine_.audio().stats();
        double const bpm = ctrl.tempo_bpm.load(std::memory_order_relaxed);
        double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
        double const ticks =
            static_cast<double>(engine_.audio().transport_position()) /
            (static_cast<double>(stats.sample_rate) * spb) * kPpq;
        double const beats_per_bar =
            static_cast<double>(ctrl.meter_num.load(std::memory_order_relaxed));
        double const bar_len_ticks = kPpq * beats_per_bar;
        long const bar = static_cast<long>(std::floor(ticks / bar_len_ticks)) + 1;
        long const beat =
            static_cast<long>(std::floor(std::fmod(ticks, bar_len_ticks) / kPpq)) + 1;
        long const tick96 = static_cast<long>(std::fmod(ticks, kPpq));
        return std::format("bar {}.beat {}.{:02}", bar, beat, tick96);
    }

    /// One value block (label above, big value below). Returns the x for
    /// the next block.
    double paint_value_block(QPainter& p, double x, char const* label,
                             QString const& value, QFont const& value_font,
                             QFont const& label_font, bool hovered,
                             char const* hint) {
        double const block_w = 56.0;
        double const h = static_cast<double>(height());
        QRectF const area{x, 0.0, block_w, h};

        if (hovered) {
            p.setPen(with_alpha(kAccent, 0.25));
            p.setBrush(with_alpha(kAccent, 0.08));
            p.drawRoundedRect(area.adjusted(1.0, 6.0, -1.0, -6.0), 6, 6);
            p.setPen(kMuted);
            p.setFont(label_font);
            p.drawText(area.adjusted(0.0, h - 18.0, 0.0, -1.0),
                       Qt::AlignHCenter | Qt::AlignTop, QString(hint));
        }

        // Label row (top), value row (below it) — no overlap.
        p.setPen(kMuted);
        p.setFont(label_font);
        p.drawText(QRectF{x, 9.0, block_w, 11.0},
                   Qt::AlignHCenter | Qt::AlignTop, QString(label));

        p.setPen(kText);
        p.setFont(value_font);
        p.drawText(QRectF{x, 21.0, block_w, 22.0},
                   Qt::AlignHCenter | Qt::AlignVCenter, value);
        return x + block_w + 10.0;
    }

    void paint_button(QPainter& p, QRectF const& btn, int index) {
        bool const hovered = btn_rects_[index].contains(mapFromGlobal(QCursor::pos()));
        bool active = false;
        QColor accent = kAccent;
        if (index == 2) {  // record: pulses while recording
            active = engine_.recording();
            accent = kRecord;
        } else if (index == 3) {  // metronome
            active = engine_.audio().transport_control().metronome_on.load(
                std::memory_order_relaxed);
        }

        QColor const fill = active ? with_alpha(accent, 0.22)
                            : hovered ? QColor(0x26, 0x2b, 0x33)
                                      : QColor(0x21, 0x25, 0x2c);
        p.setPen(QPen(active ? accent : kBorder, 1.2));
        p.setBrush(fill);
        p.drawRoundedRect(btn, 8, 8);

        QColor const icon = active ? accent : (hovered ? kText : kMuted);
        p.setPen(Qt::NoPen);
        p.setBrush(icon);
        double const s = btn.width();
        QPointF const c = btn.center();
        if (index == 0) {  // play triangle
            QPolygonF tri;
            tri << QPointF{c.x() - s * 0.14, c.y() - s * 0.20}
                << QPointF{c.x() - s * 0.14, c.y() + s * 0.20}
                << QPointF{c.x() + s * 0.20, c.y()};
            p.drawPolygon(tri);
        } else if (index == 1) {  // stop square
            p.drawRoundedRect(
                QRectF{c.x() - s * 0.14, c.y() - s * 0.14, s * 0.28, s * 0.28},
                2, 2);
        } else if (index == 2) {  // record circle (pulses while recording)
            double const pulse =
                active ? 1.0 + 0.12 * std::sin(static_cast<double>(now_ms_) / 120.0)
                       : 1.0;
            double const r = s * 0.16 * pulse;
            p.drawEllipse(c, r, r);
            if (active) {
                p.setPen(QPen(with_alpha(kRecord, 0.45), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, r + 3.0, r + 3.0);
            }
        } else {  // metronome: trapezoid body + pendulum
            QPolygonF body;
            double const half_top = s * 0.09;
            double const half_bot = s * 0.18;
            double const top = c.y() - s * 0.20;
            double const bot = c.y() + s * 0.20;
            body << QPointF{c.x() - half_top, top}
                 << QPointF{c.x() + half_top, top}
                 << QPointF{c.x() + half_bot, bot}
                 << QPointF{c.x() - half_bot, bot};
            p.drawPolygon(body);
            p.setPen(QPen(icon, 1.8, Qt::SolidLine, Qt::RoundCap));
            double const swing =
                active ? std::sin(static_cast<double>(now_ms_) / 150.0) * s * 0.08
                       : 0.0;
            p.drawLine(QPointF{c.x(), bot - 1.0},
                       QPointF{c.x() + swing, top + s * 0.06});
        }
    }

    void press_button(int index) {
        auto& ctrl = engine_.audio().transport_control();
        switch (index) {
        case 0:
            ctrl.state_req.store(music::TransportControl::kReqPlay,
                                 std::memory_order_release);
            break;
        case 1:
            ctrl.state_req.store(music::TransportControl::kReqStop,
                                 std::memory_order_release);
            break;
        case 2:
            engine_.command({mapping::Action::TransportRecord, 0, 0});
            break;
        case 3:
            ctrl.metronome_on.store(
                !ctrl.metronome_on.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            break;
        default:
            break;
        }
        update();
    }

    app::PlayEngine& engine_;
    QRectF btn_rects_[4]{};
    QRectF bpm_rect_{};
    QRectF meter_rect_{};
    QRectF loop_rect_{};
    QRectF click_rect_{};
    Hover hover_{Hover::None};
    Drag drag_{Drag::None};
    double drag_y0_{0.0};
    double drag_bpm0_{120.0};
    int drag_loop0_{0};
    std::uint64_t last_pos_{0};
    bool playing_{false};
    std::int64_t now_ms_{0};
};

// ===========================================================================
// MainWindow
// ===========================================================================
MainWindow::MainWindow(app::PlayEngine& engine, QWidget* parent)
    : QMainWindow(parent), engine_(engine) {
    setWindowTitle("addu94");
    setMinimumSize(1280, 720);
    setStyleSheet("QMainWindow { background: #14161a; }");

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(theme::kPanelGap, theme::kPanelGap,
                             theme::kPanelGap, theme::kPanelGap);
    root->setSpacing(theme::kPanelGap);

    transport_ = new TransportBar(engine_, central);
    root->addWidget(transport_);

    auto* mid = new QHBoxLayout();
    mid->setSpacing(theme::kPanelGap);
    tracks_ = new TrackStrips(engine_, central);
    tracks_->set_track_hook([](int idx, char const* param, double value) {
        log::info("ui", "mixer: track {} {} -> {}", idx, param, value);
    });
    mid->addWidget(tracks_);
    arrangement_ = new ArrangementView(engine_, central);
    mid->addWidget(arrangement_, 1);
    root->addLayout(mid, 1);

    auto* bottom = new QHBoxLayout();
    bottom->setSpacing(theme::kPanelGap);
    mirror_ = new ControllerMirror(engine_, central);
    bottom->addWidget(mirror_, 1);
    health_ = new HealthStrip(engine_, central);
    bottom->addWidget(health_);
    root->addLayout(bottom);

    setCentralWidget(central);

    // Keyboard: Space = play/stop, R = record, M = metronome.
    auto* space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, [this] { transport_->shortcut_play(); });
    auto* rec = new QShortcut(QKeySequence(Qt::Key_R), this);
    connect(rec, &QShortcut::activated, this, [this] { transport_->shortcut_record(); });
    auto* metro = new QShortcut(QKeySequence(Qt::Key_M), this);
    connect(metro, &QShortcut::activated, this,
            [this] { transport_->shortcut_metronome(); });

    ui_timer_id_ = startTimer(kUiTimerMs);
    tick();  // populate immediately (no blank first second)
}

void MainWindow::timerEvent(QTimerEvent* event) {
    if (event->timerId() == ui_timer_id_) {
        tick();
    } else {
        QMainWindow::timerEvent(event);
    }
}

void MainWindow::tick() {
    // 1. Drain app events (mapping + recorder). Pad actions carry a
    // pad index — bank A notes 36..43 are DrumPad (absolute 0..7), bank
    // B utilities/chord-slots are 8..15. Every pad hit pulses its own
    // pad in the mirror (distinct visual response per pad).
    for (auto const& c : engine_.drain_app_events()) {
        using A = mapping::Action;
        if (c.action == A::DrumPad) {
            mirror_->pulse_pad(c.param);              // bank A: 0..7
        } else if (c.action == A::ChordSlot) {
            mirror_->pulse_pad(10 + c.param);         // slots on B3..B8
        } else if (c.action == A::Undo || c.action == A::Redo) {
            mirror_->pulse_pad(c.action == A::Undo ? 8 : 9);  // B1/B2
        } else if (c.action == A::TrackSelect) {
            mirror_->pulse_pad(c.param);              // bank A rows
        }
    }

    // 2. Feed the current clip as the selected track's arrangement block.
    // Deduped on length_ticks so view-state drags/extensions survive.
    auto const& ctrl = engine_.audio().transport_control();
    std::int64_t const clip_ticks = engine_.current_clip().length_ticks;
    if (clip_ticks != last_clip_len_) {
        last_clip_len_ = clip_ticks;
        double const beats_per_bar =
            static_cast<double>(ctrl.meter_num.load(std::memory_order_relaxed));
        int const bars = std::max(
            1, static_cast<int>(std::lround(
                   static_cast<double>(clip_ticks) / (kPpq * beats_per_bar))));
        arrangement_->set_clip_block(engine_.selected_track(), bars);
    }

    // 3. Views.
    transport_->ui_tick();
    tracks_->refresh();
    arrangement_->refresh();
    mirror_->refresh();
    health_->refresh();
}

}  // namespace addu94::ui
