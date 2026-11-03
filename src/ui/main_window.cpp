// addu94 — src/ui/main_window.cpp
#include "ui/main_window.hpp"

#include "log/logger.hpp"

#include <QDoubleSpinBox>
#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimerEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace addu94::ui {

namespace {

constexpr int kUiHz = 30;                 // app-event drain / repaint rate
constexpr int kUiTimerMs = 1000 / kUiHz;  // 33 ms
[[maybe_unused]] constexpr int kTrackCount = 4;  // wired to mixer strips in M5
constexpr double kPpq = 96.0;  // ticks per quarter (music/transport.hpp)

/// Transport position (samples) → musical ticks (96 PPQ), same math as
/// play_engine.cpp's now_ticks() helper.
double position_ticks(std::uint64_t pos_samples, double bpm, double sr) noexcept {
    double const spb = 60.0 / (bpm > 0 ? bpm : 120.0);
    return static_cast<double>(pos_samples) / (sr * spb) * kPpq;
}

}  // namespace

MainWindow::MainWindow(app::PlayEngine& engine, QWidget* parent)
    : QMainWindow(parent), engine_(engine) {
    setWindowTitle("addu94");
    setMinimumSize(1280, 720);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    build_transport_bar(root);

    auto* tabs = new QTabWidget(central);

    arrangement_ = new ArrangementView(engine_, tabs);
    tabs->addTab(arrangement_, "Arrangement");

    // Pattern editor is a later milestone; the tab exists so the layout is
    // final (spec §9 keeps the hardware-first surface stable).
    auto* pattern_hint = new QLabel(
        "Pattern editor: step grid lands in M5. Record takes with pads and "
        "watch them in the Arrangement tab.", tabs);
    pattern_hint->setAlignment(Qt::AlignCenter);
    tabs->addTab(pattern_hint, "Pattern");

    mixer_ = new MixerView(tabs);
    mixer_->set_track_hook(
        [](int idx, char const* param, double value) {
            log::info("ui", "mixer: track {} {} -> {}", idx, param, value);
        });
    tabs->addTab(mixer_, "Mixer");

    device_view_ = new DeviceView(engine_, tabs);
    tabs->addTab(device_view_, "Device");

    health_view_ = new HealthView(engine_, tabs);
    tabs->addTab(health_view_, "Health");

    root->addWidget(tabs, 1);

    mirror_ = new ControllerMirror(engine_, central);
    root->addWidget(mirror_);

    setCentralWidget(central);

    ui_timer_id_ = startTimer(kUiTimerMs);
    tick();  // populate labels immediately (no blank first second)
}

void MainWindow::build_transport_bar(QVBoxLayout* root) {
    auto& ctrl = engine_.audio().transport_control();

    auto* bar = new QHBoxLayout();
    bar->setSpacing(8);
    root->addLayout(bar);

    play_btn_ = new QPushButton("Play", this);
    stop_btn_ = new QPushButton("Stop", this);
    rec_btn_ = new QPushButton("Rec", this);
    rec_btn_->setCheckable(true);
    metro_btn_ = new QPushButton("Click", this);
    metro_btn_->setCheckable(true);
    metro_btn_->setChecked(ctrl.metronome_on.load(std::memory_order_relaxed));

    // Transport state requests are consumed by the RT thread at buffer
    // boundaries; Record goes through engine.command() so the app-side
    // recorder gate toggles with it.
    connect(play_btn_, &QPushButton::clicked, this, [this] {
        engine_.audio().transport_control().state_req.store(
            music::TransportControl::kReqPlay, std::memory_order_release);
    });
    connect(stop_btn_, &QPushButton::clicked, this, [this] {
        engine_.audio().transport_control().state_req.store(
            music::TransportControl::kReqStop, std::memory_order_release);
    });
    connect(rec_btn_, &QPushButton::clicked, this, [this] {
        engine_.command({mapping::Action::TransportRecord, 0, 0});
    });
    connect(metro_btn_, &QPushButton::toggled, this, [this](bool on) {
        engine_.audio().transport_control().metronome_on.store(on,
                                                               std::memory_order_relaxed);
    });

    for (auto* b : {play_btn_, stop_btn_, rec_btn_, metro_btn_}) {
        b->setMinimumWidth(64);
        bar->addWidget(b);
    }

    bpm_ = new QDoubleSpinBox(this);
    bpm_->setRange(20.0, 333.0);
    bpm_->setDecimals(1);
    bpm_->setSingleStep(0.5);
    bpm_->setValue(ctrl.tempo_bpm.load(std::memory_order_relaxed));
    bpm_->setSuffix(" bpm");
    bpm_->setToolTip("tempo 20..333 (applied at the next buffer boundary)");
    connect(bpm_, &QDoubleSpinBox::editingFinished, this, [this] {
        engine_.audio().transport_control().tempo_bpm.store(
            bpm_->value(), std::memory_order_relaxed);
    });
    bar->addWidget(bpm_);

    loop_bars_ = new QSpinBox(this);
    loop_bars_->setRange(0, 16);
    loop_bars_->setSpecialValueText("loop off");
    loop_bars_->setSuffix(" bars");
    loop_bars_->setToolTip("loop length in bars (0 = off)");
    connect(loop_bars_, &QSpinBox::valueChanged, this, [this](int v) {
        engine_.audio().transport_control().loop_bars.store(
            static_cast<std::uint64_t>(v), std::memory_order_relaxed);
    });
    bar->addWidget(loop_bars_);

    click_level_ = new QSlider(Qt::Horizontal, this);
    click_level_->setRange(0, 100);
    click_level_->setValue(static_cast<int>(
        ctrl.metronome_gain.load(std::memory_order_relaxed) * 100.0f));
    click_level_->setFixedWidth(140);
    click_level_->setToolTip("metronome click level");
    connect(click_level_, &QSlider::valueChanged, this, [this](int v) {
        engine_.audio().transport_control().metronome_gain.store(
            static_cast<float>(v) / 100.0f, std::memory_order_relaxed);
    });
    bar->addWidget(click_level_);

    QFont mono{"monospace"};
    mono.setStyleHint(QFont::TypeWriter);

    position_label_ = new QLabel("bar 1.beat 1.00", this);
    position_label_->setFont(mono);
    position_label_->setMinimumWidth(150);
    bar->addWidget(position_label_);

    xrun_label_ = new QLabel(this);
    xrun_label_->setFont(mono);
    xrun_label_->setMinimumWidth(260);
    bar->addWidget(xrun_label_);

    device_label_ = new QLabel(this);
    device_label_->setTextFormat(Qt::RichText);
    bar->addWidget(device_label_);
    bar->addStretch(1);
}

void MainWindow::timerEvent(QTimerEvent* event) {
    if (event->timerId() == ui_timer_id_) {
        tick();
    } else {
        QMainWindow::timerEvent(event);
    }
}

void MainWindow::tick() {
    auto& ctrl = engine_.audio().transport_control();
    auto const stats = engine_.audio().stats();

    // 1. Drain app events (mapping + recorder). Pad actions carry a
    // bank-relative pad index 0..7 — mirror it to the absolute pad
    // (bank A = 0..7, bank B = 8..15) for hit feedback.
    bool const bank_b = engine_.mapping().pad_bank_b();
    for (auto const& c : engine_.drain_app_events()) {
        using A = mapping::Action;
        if (c.action == A::ChordSlot || c.action == A::ClipLaunch ||
            c.action == A::TrackSelect) {
            mirror_->pulse_pad(c.param + (bank_b ? 8 : 0));
        }
    }

    // 2. Position readout: samples → ticks → bar.beat.tick.
    double const bpm = ctrl.tempo_bpm.load(std::memory_order_relaxed);
    double const ticks =
        position_ticks(engine_.audio().transport_position(), bpm,
                       static_cast<double>(stats.sample_rate));
    double const beats_per_bar =
        static_cast<double>(ctrl.meter_num.load(std::memory_order_relaxed));
    double const bar_len_ticks = kPpq * beats_per_bar;
    long const bar =
        static_cast<long>(std::floor(ticks / bar_len_ticks)) + 1;
    long const beat =
        static_cast<long>(std::floor(std::fmod(ticks, bar_len_ticks) / kPpq)) + 1;
    long const tick96 = static_cast<long>(std::fmod(ticks, kPpq));
    position_label_->setText(QString::fromStdString(
        std::format("bar {}.beat {}.{:02}", bar, beat, tick96)));

    // 3. Stats + device LED.
    xrun_label_->setText(QString::fromStdString(std::format(
        "xruns {} | last {} µs | max {} µs", stats.xruns,
        stats.last_callback_us, stats.max_callback_us)));
    bool const connected = engine_.device_connected();
    device_label_->setText(
        QString("<span style=\"color:%1\">●</span> %2")
            .arg(connected ? "#37b26c" : "#d9534f")
            .arg(connected ? "MPK" : "no device"));

    // 4. Transport button states (no signal loops: clicked/toggled are the
    // only connections).
    rec_btn_->setChecked(engine_.recording());
    metro_btn_->setChecked(
        ctrl.metronome_on.load(std::memory_order_relaxed));

    // 5. Views.
    arrangement_->refresh();
    music::Clip& clip = engine_.current_clip();
    std::vector<ArrangementView::Block> blocks;
    if (!clip.events.empty()) {
        blocks.push_back({engine_.selected_track(), 0,
                          static_cast<int>(static_cast<double>(
                                               clip.length_ticks) /
                                           bar_len_ticks)});
    }
    arrangement_->set_blocks(std::move(blocks));
    device_view_->refresh();
    health_view_->refresh();
    mirror_->refresh();
}

}  // namespace addu94::ui
