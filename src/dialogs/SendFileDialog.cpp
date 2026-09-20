#include "dialogs/SendFileDialog.h"
#include "ui_SendFileDialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>

#include "app/Logging.h"
#include "core/LineEnding.h"

namespace {

const auto kSettingsGroup = QStringLiteral("sendFile");

QString formatBytes(qint64 bytes)
{
    if (bytes < 0) {
        bytes = 0;
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    }
    const double mb = kb / 1024.0;
    if (mb < 1024.0) {
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

QString modeToString(FileSender::Mode mode)
{
    return mode == FileSender::Mode::Binary ? QStringLiteral("binary") : QStringLiteral("text");
}

FileSender::Mode modeFromString(const QString& key)
{
    return key.compare(QStringLiteral("binary"), Qt::CaseInsensitive) == 0 ? FileSender::Mode::Binary
                                                                            : FileSender::Mode::TextLines;
}

} // namespace

SendFileDialog::SendFileDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::SendFileDialog)
    , m_sender(new FileSender(this))
{
    ui->setupUi(this);
    setWindowTitle(tr("Send File"));
    setModal(false);

    ui->lineEndingCombo->clear();
    const QList<LineEnding::Mode> modes = LineEnding::allModes();
    for (LineEnding::Mode mode : modes) {
        ui->lineEndingCombo->addItem(LineEnding::displayName(mode), static_cast<int>(mode));
    }
    ui->lineDelaySpin->setRange(0, 60000);
    ui->chunkSizeSpin->setRange(1, 65536);
    ui->chunkDelaySpin->setRange(0, 60000);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);

    // FileSender -> dialog / session
    connect(m_sender, &FileSender::chunkReady, this, &SendFileDialog::sendChunk);
    connect(m_sender, &FileSender::progress, this, &SendFileDialog::onProgress);
    connect(m_sender, &FileSender::finished, this, &SendFileDialog::onFinished);

    // Controls
    connect(ui->browseButton, &QPushButton::clicked, this, &SendFileDialog::onBrowse);
    connect(ui->startButton, &QPushButton::clicked, this, &SendFileDialog::onStart);
    connect(ui->pauseButton, &QPushButton::clicked, this, &SendFileDialog::onPauseResume);
    connect(ui->cancelButton, &QPushButton::clicked, this, &SendFileDialog::onCancel);
    connect(ui->closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(ui->modeTextRadio, &QRadioButton::toggled, this, &SendFileDialog::onModeChanged);
    connect(ui->modeBinaryRadio, &QRadioButton::toggled, this, &SendFileDialog::onModeChanged);
    connect(ui->filePathEdit, &QLineEdit::textChanged, this, [this](const QString&) { updateControls(); });

    loadOptions();
    updateControls();
}

SendFileDialog::~SendFileDialog()
{
    if (m_sender->isRunning()) {
        m_sender->cancel();
    }
    delete ui;
}

void SendFileDialog::setFilePath(const QString& path)
{
    ui->filePathEdit->setText(QDir::toNativeSeparators(path));
}

QString SendFileDialog::filePath() const
{
    return QDir::fromNativeSeparators(ui->filePathEdit->text().trimmed());
}

FileSender::Options SendFileDialog::options() const
{
    FileSender::Options opts;
    opts.filePath = filePath();
    opts.mode = ui->modeBinaryRadio->isChecked() ? FileSender::Mode::Binary : FileSender::Mode::TextLines;
    opts.lineDelayMs = ui->lineDelaySpin->value();
    opts.lineEnding = static_cast<LineEnding::Mode>(ui->lineEndingCombo->currentData().toInt());
    opts.stripLineEndings = ui->stripEndingsCheck->isChecked();
    opts.skipEmptyLines = ui->skipEmptyCheck->isChecked();
    opts.chunkSize = ui->chunkSizeSpin->value();
    opts.chunkDelayMs = ui->chunkDelaySpin->value();
    return opts;
}

bool SendFileDialog::isSending() const
{
    return m_sender->isRunning();
}

void SendFileDialog::setConnected(bool connected)
{
    if (m_connected == connected) {
        updateControls();
        return;
    }
    m_connected = connected;
    if (!m_connected && m_sender->isRunning() && !m_sender->isPaused()) {
        // Do not pour bytes into a closed port; the user can resume once reconnected.
        m_sender->pause();
        ui->statusLabel->setText(tr("Paused: the session is not connected. Press Resume once it is back."));
        qCInfo(lcUi) << "File send paused because the session disconnected";
    } else if (!m_connected && !m_sender->isRunning()) {
        ui->statusLabel->setText(tr("Not connected."));
    } else if (m_connected && !m_sender->isRunning()) {
        ui->statusLabel->setText(tr("Ready."));
    }
    updateControls();
}

void SendFileDialog::onBrowse()
{
    // Start in the directory of the current file if it exists, else in Documents.
    QString start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString current = filePath();
    if (!current.isEmpty()) {
        const QFileInfo info(current);
        if (info.exists()) {
            start = info.absoluteFilePath();
        } else if (info.dir().exists()) {
            start = info.dir().absolutePath();
        }
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select File to Send"), start,
        tr("Text and script files (*.txt *.sh *.env *.cmd *.scr);;Binary files (*.bin *.img *.hex);;All files (*)"));
    if (!path.isEmpty()) {
        setFilePath(path);
        // Guess the mode from the extension only when the user has not started anything yet.
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QStringLiteral("bin") || suffix == QStringLiteral("img")) {
            ui->modeBinaryRadio->setChecked(true);
        }
    }
}

void SendFileDialog::onStart()
{
    if (m_sender->isRunning()) {
        return;
    }
    if (!m_connected) {
        ui->statusLabel->setText(tr("Not connected."));
        return;
    }
    const FileSender::Options opts = options();
    if (opts.filePath.isEmpty()) {
        ui->statusLabel->setText(tr("Choose a file first."));
        return;
    }
    if (!QFileInfo::exists(opts.filePath)) {
        ui->statusLabel->setText(tr("File not found: %1").arg(QDir::toNativeSeparators(opts.filePath)));
        return;
    }
    saveOptions();

    ui->progressBar->setValue(0);
    if (!m_sender->start(opts)) {
        const QString error = m_sender->lastError();
        ui->statusLabel->setText(error.isEmpty() ? tr("The file could not be sent.") : error);
        qCWarning(lcUi) << "File send failed to start:" << opts.filePath << error;
        updateControls();
        return;
    }

    ui->statusLabel->setText(tr("Sending %1 ...").arg(QFileInfo(opts.filePath).fileName()));
    qCInfo(lcUi) << "File send started:" << opts.filePath << (opts.mode == FileSender::Mode::Binary ? "binary" : "text")
                 << m_sender->totalBytes() << "bytes";
    emit sendingStarted();
    updateControls();
}

void SendFileDialog::onPauseResume()
{
    if (!m_sender->isRunning()) {
        return;
    }
    if (m_sender->isPaused()) {
        if (!m_connected) {
            ui->statusLabel->setText(tr("Cannot resume: the session is not connected."));
            return;
        }
        m_sender->resume();
        ui->statusLabel->setText(tr("Resumed."));
    } else {
        m_sender->pause();
        ui->statusLabel->setText(tr("Paused."));
    }
    updateControls();
}

void SendFileDialog::onCancel()
{
    if (m_sender->isRunning()) {
        m_sender->cancel();   // emits finished(false, "Cancelled") -> onFinished()
    }
}

void SendFileDialog::onModeChanged()
{
    updateControls();
}

void SendFileDialog::onProgress(qint64 sentBytes, qint64 totalBytes, int sentLines, int totalLines)
{
    int percent = m_sender->isRunning() ? 0 : 100;
    if (totalBytes > 0) {
        const qint64 ratio = sentBytes * 100 / totalBytes;
        percent = static_cast<int>(ratio < 0 ? 0 : (ratio > 100 ? 100 : ratio));
    }
    ui->progressBar->setValue(percent);

    QString text = QStringLiteral("%1 / %2 (%3%)").arg(formatBytes(sentBytes), formatBytes(totalBytes)).arg(percent);
    if (totalLines > 0) {
        text = tr("%1 / %2 (%3%) - line %4/%5")
                   .arg(formatBytes(sentBytes), formatBytes(totalBytes))
                   .arg(percent)
                   .arg(sentLines)
                   .arg(totalLines);
    }
    if (m_sender->isPaused()) {
        text += QLatin1Char(' ') + tr("[paused]");
    }
    ui->statusLabel->setText(text);
}

void SendFileDialog::onFinished(bool completed, const QString& message)
{
    if (completed) {
        ui->progressBar->setValue(100);
    }
    ui->statusLabel->setText(message);
    qCInfo(lcUi) << "File send finished:" << (completed ? "completed" : "aborted") << message;
    emit sendingFinished(completed, message);
    updateControls();
}

void SendFileDialog::loadOptions()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    const FileSender::Options defaults;

    ui->filePathEdit->setText(QDir::toNativeSeparators(settings.value(QStringLiteral("path")).toString()));
    const FileSender::Mode mode = modeFromString(settings.value(QStringLiteral("mode"), modeToString(defaults.mode)).toString());
    ui->modeBinaryRadio->setChecked(mode == FileSender::Mode::Binary);
    ui->modeTextRadio->setChecked(mode != FileSender::Mode::Binary);
    ui->lineDelaySpin->setValue(settings.value(QStringLiteral("lineDelayMs"), defaults.lineDelayMs).toInt());
    const LineEnding::Mode ending = LineEnding::fromString(
        settings.value(QStringLiteral("lineEnding"), LineEnding::toString(defaults.lineEnding)).toString());
    const int endingIndex = ui->lineEndingCombo->findData(static_cast<int>(ending));
    ui->lineEndingCombo->setCurrentIndex(endingIndex >= 0 ? endingIndex : 0);
    ui->stripEndingsCheck->setChecked(settings.value(QStringLiteral("stripLineEndings"), defaults.stripLineEndings).toBool());
    ui->skipEmptyCheck->setChecked(settings.value(QStringLiteral("skipEmptyLines"), defaults.skipEmptyLines).toBool());
    ui->chunkSizeSpin->setValue(settings.value(QStringLiteral("chunkSize"), defaults.chunkSize).toInt());
    ui->chunkDelaySpin->setValue(settings.value(QStringLiteral("chunkDelayMs"), defaults.chunkDelayMs).toInt());
    settings.endGroup();
}

void SendFileDialog::saveOptions() const
{
    const FileSender::Options opts = options();
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(QStringLiteral("path"), opts.filePath);
    settings.setValue(QStringLiteral("mode"), modeToString(opts.mode));
    settings.setValue(QStringLiteral("lineDelayMs"), opts.lineDelayMs);
    settings.setValue(QStringLiteral("lineEnding"), LineEnding::toString(opts.lineEnding));
    settings.setValue(QStringLiteral("stripLineEndings"), opts.stripLineEndings);
    settings.setValue(QStringLiteral("skipEmptyLines"), opts.skipEmptyLines);
    settings.setValue(QStringLiteral("chunkSize"), opts.chunkSize);
    settings.setValue(QStringLiteral("chunkDelayMs"), opts.chunkDelayMs);
    settings.endGroup();
}

void SendFileDialog::updateControls()
{
    const bool sending = m_sender->isRunning();
    const bool paused = sending && m_sender->isPaused();
    const bool textMode = ui->modeTextRadio->isChecked();

    ui->filePathEdit->setEnabled(!sending);
    ui->browseButton->setEnabled(!sending);
    ui->modeTextRadio->setEnabled(!sending);
    ui->modeBinaryRadio->setEnabled(!sending);
    ui->textGroup->setEnabled(textMode && !sending);
    ui->binaryGroup->setEnabled(!textMode && !sending);

    ui->startButton->setEnabled(m_connected && !filePath().isEmpty() && !sending);
    ui->pauseButton->setEnabled(sending);
    ui->pauseButton->setText(paused ? tr("&Resume") : tr("&Pause"));
    ui->cancelButton->setEnabled(sending);
    ui->closeButton->setEnabled(true);

    if (!sending && !m_connected) {
        ui->startButton->setToolTip(tr("Connect the session first."));
    } else if (!sending && filePath().isEmpty()) {
        ui->startButton->setToolTip(tr("Choose a file to send."));
    } else {
        ui->startButton->setToolTip(QString());
    }
}
