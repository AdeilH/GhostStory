// addu94 — src/ui/health_view.cpp
#include "ui/health_view.hpp"

#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

namespace addu94::ui {

namespace {

constexpr int kBenchTailChars = 4000;

QString tail(QString const& text, int max_chars) {
    return text.size() <= max_chars
               ? text
               : text.right(max_chars).section('\n', 1);
}

}  // namespace

HealthView::HealthView(app::PlayEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
    uptime_.start();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    backend_ = new QLabel(this);
    stats_ = new QLabel(this);
    xruns_ = new QLabel(this);
    for (auto* l : {backend_, stats_, xruns_}) {
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        root->addWidget(l);
    }
    root->addSpacing(8);

    bench_btn_ = new QPushButton("Bench (10 s callback load test)", this);
    bench_btn_->setToolTip(
        "Runs `addu94 bench --seconds 10` as a child process and shows the "
        "report tail");
    root->addWidget(bench_btn_);

    bench_output_ = new QPlainTextEdit(this);
    bench_output_->setReadOnly(true);
    bench_output_->setPlaceholderText("bench output appears here…");
    root->addWidget(bench_output_, 1);

    auto* proc = new QProcess(this);
    bench_proc_ = proc;
    connect(bench_btn_, &QPushButton::clicked, this,
            [this] { start_bench(); });
    connect(proc, &QProcess::finished, this,
            [this, proc](int exit_code, QProcess::ExitStatus) {
                QString const out =
                    QString::fromUtf8(proc->readAllStandardOutput()) +
                    QString::fromUtf8(proc->readAllStandardError());
                bench_output_->setPlainText(tail(out, kBenchTailChars));
                bench_btn_->setEnabled(true);
                if (exit_code != 0) {
                    bench_output_->appendPlainText(
                        QString("\n(bench exited with code %1 — gate FAIL is "
                                "expected under load)")
                            .arg(exit_code));
                }
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError) {
                bench_output_->setPlainText(
                    QString("failed to start bench: %1")
                        .arg(proc->errorString()));
                bench_btn_->setEnabled(true);
            });

    refresh();
}

void HealthView::start_bench() {
    // Child process, not in-process: bench re-opens its own audio stream
    // and the RT budget must be measured without the GUI in the loop.
    if (bench_proc_ == nullptr) return;
    bench_btn_->setEnabled(false);
    bench_output_->setPlainText("running bench --seconds 10 …");
    bench_proc_->start(QStringLiteral("/proc/self/exe"),
                       QStringList{"bench", "--seconds", "10"});
}

void HealthView::refresh() {
    auto const stats = engine_.audio().stats();
    backend_->setText(
        QString("backend: %1 | audio %2")
            .arg(QString::fromStdString(engine_.audio().backend_name()))
            .arg(engine_.audio_running() ? "running" : "STOPPED"));

    qint64 const up_s = uptime_.elapsed() / 1000;
    stats_->setText(QString("callbacks: %1 | last: %2 µs | max: %3 µs | "
                            "rate: %4 Hz | buffer: %5 frames")
                        .arg(static_cast<qulonglong>(stats.callbacks))
                        .arg(stats.last_callback_us)
                        .arg(stats.max_callback_us)
                        .arg(stats.sample_rate)
                        .arg(stats.buffer_size));

    xruns_->setText(QString("xruns: %1 | gui uptime: %2:%3:%4")
                        .arg(static_cast<qulonglong>(stats.xruns))
                        .arg(up_s / 3600, 2, 10, QLatin1Char('0'))
                        .arg((up_s / 60) % 60, 2, 10, QLatin1Char('0'))
                        .arg(up_s % 60, 2, 10, QLatin1Char('0')));
}

}  // namespace addu94::ui
