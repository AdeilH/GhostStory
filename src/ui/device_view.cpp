// addu94 — src/ui/device_view.cpp
#include "ui/device_view.hpp"

#include "device/manifest.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fstream>
#include <iterator>
#include <string>

namespace addu94::ui {

namespace {

constexpr int kKnobCount = 8;

}  // namespace

DeviceView::DeviceView(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    status_ = new QLabel(this);
    status_->setTextFormat(Qt::RichText);
    root->addWidget(status_);

    auto* form = new QFormLayout();
    velocity_curve_ = new QComboBox(this);
    velocity_curve_->addItems({"Linear", "Log", "Exp", "Hard-floor"});
    velocity_curve_->setToolTip(
        "Velocity curve for the instrument path (stored in the UI until M6 "
        "wires the synth-side curve)");
    form->addRow("Velocity curve:", velocity_curve_);
    root->addLayout(form);

    root->addWidget(new QLabel("Knobs (live mirror values 0..127)", this));
    knobs_ = new QTableWidget(kKnobCount, 2, this);
    knobs_->setHorizontalHeaderLabels({"Knob", "Value"});
    knobs_->verticalHeader()->setVisible(false);
    knobs_->horizontalHeader()->setStretchLastSection(true);
    knobs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    knobs_->setMaximumHeight(220);
    for (int i = 0; i < kKnobCount; ++i) {
        auto* name = new QTableWidgetItem(QString("Knob %1").arg(i + 1));
        auto* value = new QTableWidgetItem("0");
        knobs_->setItem(i, 0, name);
        knobs_->setItem(i, 1, value);
    }
    root->addWidget(knobs_);

    root->addWidget(new QLabel("Probe summary", this));
    summary_ = new QPlainTextEdit(this);
    summary_->setReadOnly(true);
    summary_->setMaximumHeight(160);
    root->addWidget(summary_);

    load_manifest_summary();
    refresh();
}

void DeviceView::load_manifest_summary() {
    // The manifest is a plain data file (spec §1): read + parse via the
    // same checksummed path the probe uses.
    std::ifstream in(device::manifest_path());
    if (!in) {
        summary_->setPlainText(
            "no device manifest at ~/.local/share/addu94/devices/\n"
            "run addu94 probe");
        return;
    }
    std::string const text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    auto const m = device::manifest_from_json(text);
    if (!m) {
        summary_->setPlainText(
            "device manifest unreadable (corrupt — quarantined on next "
            "probe); run addu94 probe");
        return;
    }
    std::size_t pads = 0;
    std::size_t knobs = 0;
    for (auto const& c : m->controls) {
        if (c.kind == device::Control::Kind::Pad) ++pads;
        if (c.kind == device::Control::Kind::Knob) ++knobs;
    }
    summary_->setPlainText(QString(
        "model: %1\nfirmware: %2\nalsa client: %3 (%4)\nports: %5\n"
        "controls: %6 (%7 pads, %8 knobs)\nidentity reply: %9")
                               .arg(QString::fromStdString(
                                   m->model.empty() ? "unidentified" : m->model))
                               .arg(QString::fromStdString(
                                   m->firmware.empty() ? "-" : m->firmware))
                               .arg(m->alsa_client)
                               .arg(QString::fromStdString(m->alsa_client_name))
                               .arg(static_cast<int>(m->ports.size()))
                               .arg(static_cast<int>(m->controls.size()))
                               .arg(static_cast<int>(pads))
                               .arg(static_cast<int>(knobs))
                               .arg(m->identity_request_supported ? "yes"
                                                                  : "no"));
}

void DeviceView::refresh() {
    bool const ok = engine_.device_connected();
    status_->setText(QString("<span style=\"color:%1; font-weight:bold\">"
                             "%2</span>  MPK Mini %3")
                         .arg(ok ? "#37b26c" : "#d9534f")
                         .arg(ok ? "CONNECTED" : "OFFLINE")
                         .arg(ok ? "— keys, pads and knobs are live"
                                 : "— plug in via USB, auto-reconnect runs"));
    for (int i = 0; i < kKnobCount; ++i) {
        if (auto* item = knobs_->item(i, 1)) {
            item->setText(QString::number(engine_.mapping().knob_value(i)));
        }
    }
}

}  // namespace addu94::ui
