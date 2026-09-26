#include "dialogs/RemoteFileDialog.h"
#include "ui_RemoteFileDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStandardPaths>
#include <QToolButton>

#include "app/Logging.h"

namespace {

const auto kSettingsGroup = QStringLiteral("remoteFile");
const auto kLastLocalKey = QStringLiteral("lastLocalPath");
const auto kLastRemoteKey = QStringLiteral("lastRemotePath");

QString downloadsDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

/// "/oem/app.bin" -> "app.bin"; "app.bin" -> "app.bin"; "/oem/" -> "".
QString remoteFileName(const QString& remotePath)
{
    const qsizetype slash = remotePath.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? remotePath.mid(slash + 1) : remotePath;
}

/// "/oem/app.bin" -> "/oem/"; "app.bin" -> "".
QString remoteDirectory(const QString& remotePath)
{
    const qsizetype slash = remotePath.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? remotePath.left(slash + 1) : QString();
}

QString joinRemote(const QString& dir, const QString& name)
{
    if (dir.isEmpty()) {
        return name;
    }
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes < 0 ? 0 : bytes);
}

} // namespace

RemoteFileDialog::RemoteFileDialog(SshConnection* connection, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::RemoteFileDialog)
    , m_connection(connection)
{
    ui->setupUi(this);
    setWindowTitle(tr("Remote File Transfer"));
    setModal(false);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);

    if (m_connection) {
        m_remoteHome = m_connection->remoteHome();
        connect(m_connection, &Transport::stateChanged, this, &RemoteFileDialog::onConnectionState);
        connect(m_connection, &SshConnection::transferStarted, this, &RemoteFileDialog::onTransferStarted);
        connect(m_connection, &SshConnection::transferProgress, this, &RemoteFileDialog::onProgress);
        connect(m_connection, &SshConnection::transferFinished, this, &RemoteFileDialog::onFinished);
        connect(m_connection, &SshConnection::remoteHomeReceived, this, &RemoteFileDialog::onRemoteHomeReceived);
    }

    connect(ui->radioUpload, &QRadioButton::toggled, this, [this](bool on) {
        if (on) {
            onDirectionChanged();
        }
    });
    connect(ui->radioDownload, &QRadioButton::toggled, this, [this](bool on) {
        if (on) {
            onDirectionChanged();
        }
    });
    connect(ui->buttonBrowseLocal, &QPushButton::clicked, this, &RemoteFileDialog::onBrowseLocal);
    connect(ui->buttonRemoteHome, &QToolButton::clicked, this, &RemoteFileDialog::onRemoteHome);
    connect(ui->buttonStart, &QPushButton::clicked, this, &RemoteFileDialog::onStart);
    connect(ui->buttonCancel, &QPushButton::clicked, this, &RemoteFileDialog::onCancel);
    connect(ui->buttonClose, &QPushButton::clicked, this, &QDialog::close);

    // Typing into a field makes it the user's own; a derived value may be replaced.
    connect(ui->editLocalPath, &QLineEdit::textEdited, this, [this](const QString&) { m_localDerived = false; });
    connect(ui->editRemotePath, &QLineEdit::textEdited, this, [this](const QString&) { m_remoteDerived = false; });
    connect(ui->editLocalPath, &QLineEdit::textChanged, this, [this](const QString&) { updateControls(); });
    connect(ui->editRemotePath, &QLineEdit::textChanged, this, [this](const QString&) { updateControls(); });
    connect(ui->editLocalPath, &QLineEdit::editingFinished, this, [this]() { applyDefaults(); });
    connect(ui->editRemotePath, &QLineEdit::editingFinished, this, [this]() { applyDefaults(); });

    loadPaths();
    updateDirectionUi();
    setIdleStatus();
    updateControls();
}

RemoteFileDialog::~RemoteFileDialog()
{
    savePaths();
    delete ui;
}

void RemoteFileDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        // retranslateUi() resets the captions and the .ui default of labelStatus; rebuild the
        // direction-dependent texts and the idle line (a running transfer keeps its progress
        // text, the next progress signal rewrites it anyway).
        ui->retranslateUi(this);
        setWindowTitle(tr("Remote File Transfer"));
        updateDirectionUi();
        if (!(m_connection && m_connection->isTransferActive())) {
            setIdleStatus();
        }
        updateControls();
    }
    QDialog::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void RemoteFileDialog::setDirection(SshConnection::TransferDirection direction)
{
    if (direction == SshConnection::TransferDirection::Upload) {
        ui->radioUpload->setChecked(true);
    } else {
        ui->radioDownload->setChecked(true);
    }
}

void RemoteFileDialog::setLocalPath(const QString& path)
{
    setLocalText(path, false);
    applyDefaults();
}

void RemoteFileDialog::setRemotePath(const QString& path)
{
    setRemoteText(path, false);
    applyDefaults();
}

SshConnection::TransferRequest RemoteFileDialog::request() const
{
    SshConnection::TransferRequest req;
    req.direction =
        isUpload() ? SshConnection::TransferDirection::Upload : SshConnection::TransferDirection::Download;
    req.localPath = localPath();
    req.remotePath = remotePath();
    req.overwrite = ui->checkOverwrite->isChecked();
    req.preservePermissions = ui->checkPreservePermissions->isChecked();
    return req;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool RemoteFileDialog::isUpload() const
{
    return ui->radioUpload->isChecked();
}

QString RemoteFileDialog::localPath() const
{
    return QDir::fromNativeSeparators(ui->editLocalPath->text().trimmed());
}

QString RemoteFileDialog::remotePath() const
{
    return ui->editRemotePath->text().trimmed();
}

void RemoteFileDialog::setLocalText(const QString& path, bool derived)
{
    ui->editLocalPath->setText(QDir::toNativeSeparators(path.trimmed()));
    m_localDerived = derived;
}

void RemoteFileDialog::setRemoteText(const QString& path, bool derived)
{
    ui->editRemotePath->setText(path.trimmed());
    m_remoteDerived = derived;
}

void RemoteFileDialog::applyDefaults()
{
    if (isUpload()) {
        // Remote destination = <remote home or the typed directory>/<local file name>.
        const QString name = QFileInfo(localPath()).fileName();
        if (name.isEmpty()) {
            return;
        }
        const QString remote = remotePath();
        const bool directoryOnly = remote.endsWith(QLatin1Char('/'));
        if (!(remote.isEmpty() || directoryOnly || m_remoteDerived)) {
            return;   // typed by the user: keep it
        }
        QString dir;
        if (directoryOnly) {
            dir = remote;
        } else if (m_remoteDerived && !remoteDirectory(remote).isEmpty()) {
            dir = remoteDirectory(remote);   // keep the directory of the previous derived value
        } else {
            dir = m_remoteHome.isEmpty() && m_connection ? m_connection->remoteHome() : m_remoteHome;
        }
        if (dir.isEmpty()) {
            // The home is not known yet: ask for it, onRemoteHomeReceived() completes the path.
            if (m_connection && m_connection->isOpen()) {
                m_connection->requestRemoteHome();
            }
            return;
        }
        setRemoteText(joinRemote(dir, name), true);
    } else {
        // Local destination = <Downloads or the chosen directory>/<remote file name>.
        const QString name = remoteFileName(remotePath());
        if (name.isEmpty()) {
            return;
        }
        const QString local = localPath();
        const bool isDirectory = !local.isEmpty() && QFileInfo(local).isDir();
        if (!(local.isEmpty() || isDirectory || m_localDerived)) {
            return;   // typed by the user: keep it
        }
        QString dir;
        if (isDirectory) {
            dir = local;
        } else if (const QString previous = QFileInfo(local).path();
                   m_localDerived && !previous.isEmpty() && previous != QLatin1String(".")) {
            dir = previous;   // keep the directory of the previous derived value
        } else {
            dir = downloadsDirectory();
        }
        setLocalText(QDir(dir).filePath(name), true);
    }
}

void RemoteFileDialog::fillRemoteHome(const QString& home)
{
    const QString name = isUpload() ? QFileInfo(localPath()).fileName() : remoteFileName(remotePath());
    QString path = home.endsWith(QLatin1Char('/')) ? home : home + QLatin1Char('/');
    if (!name.isEmpty()) {
        path += name;
    }
    setRemoteText(path, isUpload());
    if (!isUpload()) {
        applyDefaults();   // a new remote file name may complete the local path
    }
}

void RemoteFileDialog::loadPaths()
{
    // Restored as derived values: nobody typed them into this dialog. A file dropped or
    // browsed later keeps their directory but gets its own name (applyDefaults()), instead of
    // being written over the previous session's file.
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    setLocalText(settings.value(kLastLocalKey).toString(), true);
    setRemoteText(settings.value(kLastRemoteKey).toString(), true);
    settings.endGroup();
}

void RemoteFileDialog::savePaths() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kLastLocalKey, localPath());
    settings.setValue(kLastRemoteKey, remotePath());
    settings.endGroup();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void RemoteFileDialog::onBrowseLocal()
{
    QString start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString current = localPath();
    if (!current.isEmpty()) {
        const QFileInfo info(current);
        if (info.exists()) {
            start = info.absoluteFilePath();
        } else if (info.dir().exists()) {
            start = info.dir().absolutePath();
        }
    }
    QString path;
    if (isUpload()) {
        path = QFileDialog::getOpenFileName(this, tr("Select File to Upload"), start, tr("All files (*)"));
    } else {
        if (current.isEmpty()) {
            start = QDir(downloadsDirectory()).filePath(remoteFileName(remotePath()));
        }
        path = QFileDialog::getSaveFileName(this, tr("Save Downloaded File As"), start, tr("All files (*)"));
    }
    if (!path.isEmpty()) {
        setLocalPath(path);
    }
}

void RemoteFileDialog::onRemoteHome()
{
    QString home = m_remoteHome;
    if (home.isEmpty() && m_connection) {
        home = m_connection->remoteHome();
    }
    if (home.isEmpty()) {
        if (m_connection && m_connection->isOpen()) {
            m_homeWanted = true;
            m_connection->requestRemoteHome();
            ui->labelStatus->setText(tr("Resolving the remote home directory..."));
        } else {
            ui->labelStatus->setText(tr("The remote home directory is known once the session is connected."));
        }
        return;
    }
    fillRemoteHome(home);
}

void RemoteFileDialog::onRemoteHomeReceived(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }
    m_remoteHome = path;
    if (m_homeWanted) {
        m_homeWanted = false;
        fillRemoteHome(path);
        if (!(m_connection && m_connection->isTransferActive())) {
            setIdleStatus();
        }
    } else {
        applyDefaults();
    }
}

void RemoteFileDialog::onDirectionChanged()
{
    updateDirectionUi();
    // The roles swap: the previous source is now the destination candidate. A path chosen as a
    // source (browsed, dropped or typed) was never meant as a destination, so it becomes derived
    // - applyDefaults() keeps its directory and lets the file name follow the new source.
    if (isUpload()) {
        m_remoteDerived = true;
    } else {
        m_localDerived = true;
    }
    applyDefaults();
    if (!(m_connection && m_connection->isTransferActive())) {
        setIdleStatus();
    }
    updateControls();
}

void RemoteFileDialog::updateDirectionUi()
{
    if (isUpload()) {
        ui->labelLocal->setText(tr("&Local file:"));
        ui->labelRemote->setText(tr("&Remote path:"));
        ui->editLocalPath->setToolTip(tr("The file to upload"));
        ui->editRemotePath->setToolTip(
            tr("Where to write it on the remote host (a directory ending in / keeps the file name)"));
        // "&Start ..." rather than "&Upload": the radio button already owns Alt+U (Alt+D below).
        ui->buttonStart->setText(tr("&Start upload"));
    } else {
        ui->labelLocal->setText(tr("&Save as:"));
        ui->labelRemote->setText(tr("&Remote file:"));
        ui->editLocalPath->setToolTip(
            tr("Where to write the downloaded file (an existing directory keeps the file name)"));
        ui->editRemotePath->setToolTip(tr("The remote file to download"));
        ui->buttonStart->setText(tr("&Start download"));
    }
}

void RemoteFileDialog::onStart()
{
    if (!m_connection) {
        return;
    }
    const SshConnection::TransferRequest req = request();
    if (req.localPath.isEmpty() || req.remotePath.isEmpty()) {
        ui->labelStatus->setText(tr("Enter both the local and the remote path."));
        return;
    }
    const bool upload = (req.direction == SshConnection::TransferDirection::Upload);
    if (upload && !QFileInfo(req.localPath).isFile()) {
        ui->labelStatus->setText(tr("Local file not found: %1").arg(QDir::toNativeSeparators(req.localPath)));
        return;
    }
    if (!upload && !req.overwrite && QFileInfo::exists(req.localPath)) {
        ui->labelStatus->setText(
            tr("%1 already exists (enable Overwrite to replace it).").arg(QDir::toNativeSeparators(req.localPath)));
        return;
    }
    savePaths();

    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    if (!m_connection->startTransfer(req)) {
        ui->labelStatus->setText(
            tr("The transfer could not be started (not connected, or another transfer is running)."));
        qCWarning(lcSsh) << "SFTP transfer refused:" << req.localPath << "<->" << req.remotePath;
        updateControls();
        return;
    }
    ui->labelStatus->setText(upload ? tr("Uploading %1...").arg(QFileInfo(req.localPath).fileName())
                                    : tr("Downloading %1...").arg(remoteFileName(req.remotePath)));
    qCInfo(lcSsh) << (upload ? "upload started:" : "download started:") << req.localPath << "<->" << req.remotePath;
    updateControls();
}

void RemoteFileDialog::onCancel()
{
    if (m_connection && m_connection->isTransferActive()) {
        m_connection->cancelTransfer();
        ui->labelStatus->setText(tr("Cancelling..."));
    }
}

void RemoteFileDialog::onTransferStarted(const SshConnection::TransferRequest& req)
{
    const bool upload = (req.direction == SshConnection::TransferDirection::Upload);
    ui->labelStatus->setText(upload ? tr("Uploading %1...").arg(QFileInfo(req.localPath).fileName())
                                    : tr("Downloading %1...").arg(remoteFileName(req.remotePath)));
    updateControls();
}

void RemoteFileDialog::onProgress(qint64 done, qint64 total)
{
    if (total > 0) {
        const qint64 ratio = done * 100 / total;
        const int percent = static_cast<int>(ratio < 0 ? 0 : (ratio > 100 ? 100 : ratio));
        ui->progressBar->setRange(0, 100);
        ui->progressBar->setValue(percent);
        ui->labelStatus->setText(QStringLiteral("%1 / %2 (%3%)").arg(formatSize(done), formatSize(total)).arg(percent));
    } else {
        ui->progressBar->setRange(0, 0);   // size unknown: busy indicator
        ui->labelStatus->setText(formatSize(done));
    }
}

void RemoteFileDialog::onFinished(bool ok, const QString& message)
{
    ui->progressBar->setRange(0, 100);
    if (ok) {
        ui->progressBar->setValue(100);
    }
    ui->labelStatus->setText(message.isEmpty() ? (ok ? tr("Transfer complete.") : tr("Transfer failed.")) : message);
    qCInfo(lcSsh) << "SFTP transfer finished:" << (ok ? "ok" : "failed") << message;
    emit transferFinished(ok, message);
    updateControls();
}

void RemoteFileDialog::onConnectionState(Transport::State state)
{
    if (!(m_connection && m_connection->isTransferActive())) {
        setIdleStatus();
    }
    if (state != Transport::State::Connected) {
        m_homeWanted = false;
    }
    updateControls();
}

void RemoteFileDialog::setIdleStatus()
{
    const bool connected = m_connection && m_connection->state() == Transport::State::Connected;
    ui->labelStatus->setText(connected ? tr("Ready.") : tr("Not connected."));
}

void RemoteFileDialog::updateControls()
{
    const bool connected = m_connection && m_connection->state() == Transport::State::Connected;
    const bool active = m_connection && m_connection->isTransferActive();
    const bool havePaths = !localPath().isEmpty() && !remotePath().isEmpty();

    const QList<QWidget*> inputs = {ui->radioUpload,      ui->radioDownload,   ui->editLocalPath,
                                    ui->buttonBrowseLocal, ui->editRemotePath, ui->buttonRemoteHome,
                                    ui->checkOverwrite,   ui->checkPreservePermissions};
    for (QWidget* widget : inputs) {
        widget->setEnabled(!active);
    }
    ui->buttonStart->setEnabled(connected && havePaths && !active);
    ui->buttonCancel->setEnabled(active);
    ui->buttonClose->setEnabled(true);

    if (active) {
        ui->buttonStart->setToolTip(tr("A transfer is already running."));
    } else if (!connected) {
        ui->buttonStart->setToolTip(tr("Connect the session first."));
    } else if (!havePaths) {
        ui->buttonStart->setToolTip(tr("Enter the local and the remote path."));
    } else {
        ui->buttonStart->setToolTip(QString());
    }
}
