// addu94 — src/ui/mixer_view.cpp
#include "ui/mixer_view.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <utility>

namespace addu94::ui {

namespace {
constexpr int kTrackCount = 4;
}  // namespace

MixerView::MixerView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    auto* row = new QHBoxLayout();
    row->setSpacing(10);
    root->addLayout(row);
    root->addStretch(1);

    for (int t = 0; t < kTrackCount; ++t) {
        auto* strip = new QVBoxLayout();
        strip->setSpacing(6);

        auto* name = new QLabel(QString("Track %1").arg(t + 1), this);
        name->setAlignment(Qt::AlignHCenter);
        strip->addWidget(name);

        auto* vol = new QSlider(Qt::Vertical, this);
        vol->setRange(0, 150);
        vol->setValue(100);
        vol->setTickPosition(QSlider::TicksRight);
        vol->setMinimumHeight(180);
        strip->addWidget(vol, 1);

        auto* pan = new QSlider(Qt::Horizontal, this);
        pan->setRange(-100, 100);
        pan->setValue(0);
        strip->addWidget(pan);

        auto* toggles = new QHBoxLayout();
        auto* mute = new QPushButton("M", this);
        auto* solo = new QPushButton("S", this);
        mute->setCheckable(true);
        solo->setCheckable(true);
        mute->setToolTip(QString("mute track %1").arg(t + 1));
        solo->setToolTip(QString("solo track %1").arg(t + 1));
        toggles->addWidget(mute);
        toggles->addWidget(solo);
        strip->addLayout(toggles);

        row->addLayout(strip);

        volume_.push_back(vol);
        pan_.push_back(pan);
        mute_.push_back(mute);
        solo_.push_back(solo);

        connect(vol, &QSlider::valueChanged, this,
                [this, t](int v) { if (hook_) hook_(t, "volume", v); });
        connect(pan, &QSlider::valueChanged, this,
                [this, t](int v) { if (hook_) hook_(t, "pan", v); });
        connect(mute, &QPushButton::toggled, this,
                [this, t](bool on) { if (hook_) hook_(t, "mute", on ? 1.0 : 0.0); });
        connect(solo, &QPushButton::toggled, this,
                [this, t](bool on) { if (hook_) hook_(t, "solo", on ? 1.0 : 0.0); });
    }
}

void MixerView::set_track_hook(TrackHook hook) {
    hook_ = std::move(hook);
}

}  // namespace addu94::ui
