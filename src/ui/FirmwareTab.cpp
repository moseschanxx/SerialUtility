// STUB (v0.5 scaffolding): replaced by the ui work package. Builds and shows an empty widget.
#include "ui/FirmwareTab.h"

#include <QLabel>
#include <QVBoxLayout>

#include "firmware/McuLink.h"
#include "firmware/ToolRunner.h"
#include "ssh/SshConnection.h"

struct FirmwareTab::Private
{
    ToolRunner* runner = nullptr;
    SocFlasher* flasher = nullptr;
    McuLink* link = nullptr;
    McuUpdater* updater = nullptr;
    ImagePuller* puller = nullptr;
    SshConnection* ssh = nullptr;
    SessionProvider sessions;
    int refreshMs = 2000;
    QString target, remoteFolder, cacheDir, imagePath, packagePath;
    quint32 role = 0;
};

FirmwareTab::FirmwareTab(SshProfileStore*, ToolRunner* runner, QWidget* parent) : QWidget(parent), d(new Private)
{
    d->runner = runner ? runner : new ProcessToolRunner(this);
    d->flasher = new SocFlasher(d->runner, this);
    d->link = new McuLink(d->runner, this);
    d->updater = new McuUpdater(this);
    d->puller = new ImagePuller(this);
    d->ssh = new SshConnection(this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Firmware tab: not implemented yet"), this));
}
FirmwareTab::~FirmwareTab() = default;
QString FirmwareTab::title() const { return tr("Firmware"); }
bool FirmwareTab::isBusy() const { return false; }
QString FirmwareTab::persistentStatusMessage() const { return QString(); }
QString FirmwareTab::currentOperation() const { return QString(); }
ToolRunner* FirmwareTab::toolRunner() const { return d->runner; }
SocFlasher* FirmwareTab::socFlasher() const { return d->flasher; }
McuLink* FirmwareTab::mcuLink() const { return d->link; }
McuUpdater* FirmwareTab::mcuUpdater() const { return d->updater; }
ImagePuller* FirmwareTab::imagePuller() const { return d->puller; }
SshConnection* FirmwareTab::sshConnection() const { return d->ssh; }
void FirmwareTab::setToolPaths(const QString&, const QString&, const QString&) {}
void FirmwareTab::setSessionProvider(SessionProvider provider) { d->sessions = std::move(provider); }
void FirmwareTab::setStatusRefreshIntervalMs(int ms) { d->refreshMs = ms; }
int FirmwareTab::statusRefreshIntervalMs() const { return d->refreshMs; }
QString FirmwareTab::sourceTarget() const { return d->target; }
void FirmwareTab::setSourceTarget(const QString& t) { d->target = t; }
QString FirmwareTab::remoteFolder() const { return d->remoteFolder; }
void FirmwareTab::setRemoteFolder(const QString& f) { d->remoteFolder = f; }
QString FirmwareTab::cacheDirectory() const { return d->cacheDir; }
void FirmwareTab::setCacheDirectory(const QString& dir) { d->cacheDir = dir; }
QString FirmwareTab::imagePath() const { return d->imagePath; }
void FirmwareTab::setImagePath(const QString& path) { d->imagePath = path; }
QString FirmwareTab::packagePath() const { return d->packagePath; }
bool FirmwareTab::setPackagePath(const QString& path) { d->packagePath = path; return false; }
CcPackage FirmwareTab::package() const { return {}; }
quint32 FirmwareTab::selectedRole() const { return d->role; }
void FirmwareTab::setSelectedRole(quint32 role) { d->role = role; }
QString FirmwareTab::runningMcuVersion() const { return QString(); }
QString FirmwareTab::runningMcuRole() const { return QString(); }
QComboBox* FirmwareTab::sourceCombo() const { return nullptr; }
QLineEdit* FirmwareTab::remoteFolderEdit() const { return nullptr; }
QLineEdit* FirmwareTab::cacheDirEdit() const { return nullptr; }
QPushButton* FirmwareTab::pullButton() const { return nullptr; }
QPushButton* FirmwareTab::localImageButton() const { return nullptr; }
QLabel* FirmwareTab::imageInfoLabel() const { return nullptr; }
QPushButton* FirmwareTab::rollbackButton() const { return nullptr; }
QLabel* FirmwareTab::socStatusLabel() const { return nullptr; }
QLabel* FirmwareTab::socImageLabel() const { return nullptr; }
QPushButton* FirmwareTab::flashSocButton() const { return nullptr; }
QPushButton* FirmwareTab::cancelButton() const { return nullptr; }
QLineEdit* FirmwareTab::packageEdit() const { return nullptr; }
QPushButton* FirmwareTab::packageButton() const { return nullptr; }
QLabel* FirmwareTab::packageInfoLabel() const { return nullptr; }
QLabel* FirmwareTab::mcuStatusLabel() const { return nullptr; }
QPushButton* FirmwareTab::readMcuButton() const { return nullptr; }
QRadioButton* FirmwareTab::roleRxRadio() const { return nullptr; }
QRadioButton* FirmwareTab::roleTxRadio() const { return nullptr; }
QPushButton* FirmwareTab::updateMcuButton() const { return nullptr; }
QPushButton* FirmwareTab::setRoleButton() const { return nullptr; }
QPushButton* FirmwareTab::pullAndFlashButton() const { return nullptr; }
QProgressBar* FirmwareTab::progressBar() const { return nullptr; }
QLabel* FirmwareTab::progressLabel() const { return nullptr; }
QPlainTextEdit* FirmwareTab::logView() const { return nullptr; }
QPushButton* FirmwareTab::clearLogButton() const { return nullptr; }
QPushButton* FirmwareTab::saveLogButton() const { return nullptr; }
QPushButton* FirmwareTab::installDriversButton() const { return nullptr; }
void FirmwareTab::pull() {}
void FirmwareTab::flashSoc() {}
void FirmwareTab::updateMcu() {}
void FirmwareTab::setMcuRole() {}
void FirmwareTab::readMcuInfo() {}
void FirmwareTab::pullAndFlash() {}
void FirmwareTab::cancel() {}
void FirmwareTab::rollBack() {}
void FirmwareTab::chooseLocalImage() {}
void FirmwareTab::choosePackage() {}
void FirmwareTab::refreshStatus() {}
void FirmwareTab::installDrivers() {}
void FirmwareTab::appendLog(const QString& line) { emit logLineAdded(line); }
void FirmwareTab::clearLog() {}
void FirmwareTab::saveLog() {}
void FirmwareTab::applyPreferences() {}
void FirmwareTab::disconnectSource() {}
void FirmwareTab::showEvent(QShowEvent* event) { QWidget::showEvent(event); }
void FirmwareTab::hideEvent(QHideEvent* event) { QWidget::hideEvent(event); }
void FirmwareTab::changeEvent(QEvent* event) { QWidget::changeEvent(event); }
