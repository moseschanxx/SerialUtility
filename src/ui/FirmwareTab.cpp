#include "ui/FirmwareTab.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "dialogs/AuthPromptDialog.h"
#include "dialogs/HostKeyDialog.h"
#include "firmware/CcProtocol.h"
#include "firmware/McuLink.h"
#include "firmware/RockchipTool.h"
#include "firmware/ToolRunner.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "ui/SessionWidget.h"

namespace {

constexpr int kStatusShortMs = 3000;
constexpr int kStatusLongMs = 5000;
constexpr int kDefaultRefreshMs = 2000;
constexpr int kFirstRefreshDelayMs = 500;   ///< the first status query after the tab became visible
constexpr int kLogMaxBlocks = 20000;
/// A collaborator whose start() refused gets this long to report the reason through its own
/// finished()/failed() (they report "from the event loop") before the tab ends the step itself.
constexpr int kSilentRefusalMs = 100;
/// McuLink::close() reports closed() when adb is done; never wait longer than this for it.
constexpr int kLinkCloseTimeoutMs = 10000;
/// A cancelled download ends once the connection removed its ".part"; never wait longer than this.
constexpr int kPullCancelGraceMs = 5000;
constexpr int kTargetComboMinWidth = 220;

// Operation / step names (currentOperation(), operationFinished()).
const QLatin1String kOpPull("pull");
const QLatin1String kOpFlash("flash");
const QLatin1String kOpMcu("mcu");
const QLatin1String kOpSetRole("set-role");
const QLatin1String kOpInfo("info");
const QLatin1String kOpPullFlash("pull-flash");

// The restore-key prefixes of SessionWidget::restoreKey(), accepted by setSourceTarget().
const QLatin1String kProfileKeyPrefix("ssh:profile:");
const QLatin1String kTargetKeyPrefix("ssh:target:");

/// AppSettings.h has no accessor for the confirmed MCU role, so it lives next to the other
/// firmware keys in plain QSettings ("rx" / "tx"; empty = none confirmed).
const QLatin1String kRoleSettingsKey("firmware/role");

const QColor kErrorText(0xC0, 0x39, 0x2B);

/// Combo text of a stored profile, exactly as SshConnectionBar shows it: "name - user@host:port".
QString profileItemText(const SshProfile& profile)
{
    const QString name = profile.name.trimmed();
    const QString target = profile.displayTarget();
    if (name.isEmpty() || name == target) {
        return target;
    }
    return QStringLiteral("%1 - %2").arg(name, target);
}

/// QComboBox::insertSeparator() marks the item with this accessible description.
bool isSeparator(const QComboBox* combo, int index)
{
    return combo->itemData(index, Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator");
}

/// The recent-target item (no profile id, not a separator) whose text equals `text`, or -1.
int findRecentItem(const QComboBox* combo, const QString& text)
{
    for (int i = 0; i < combo->count(); ++i) {
        if (!isSeparator(combo, i) && combo->itemData(i).toString().isEmpty() && combo->itemText(i) == text) {
            return i;
        }
    }
    return -1;
}

QString formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeTraditionalFormat);
}

QString formatTime(const QDateTime& time)
{
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
}

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

QString roleText(quint32 role)
{
    switch (role) {
    case CcProtocol::RoleRx:
        return QStringLiteral("rx");
    case CcProtocol::RoleTx:
        return QStringLiteral("tx");
    default:
        return QString();
    }
}

quint32 roleFromText(const QString& text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("rx")) {
        return CcProtocol::RoleRx;
    }
    if (t == QLatin1String("tx")) {
        return CcProtocol::RoleTx;
    }
    return 0;
}

/// The monospace log font of the System Log dock, so both panes read alike.
QFont logFont()
{
    QFont font(QStringLiteral("Consolas"), 9);
    font.setStyleHint(QFont::Monospace);
    if (!QFontInfo(font).fixedPitch()) {
        font = QFont(QStringLiteral("Courier New"), 9);
        font.setStyleHint(QFont::Monospace);
    }
    return font;
}

} // namespace

// =======================================================================================
// Private
// =======================================================================================

struct FirmwareTab::Private
{
    explicit Private(FirmwareTab* owner) : q(owner) {}

    enum class PullPhase { Idle, Connecting, Pulling };

    FirmwareTab* q;

    // ---- Collaborators ---------------------------------------------------------------------
    SshProfileStore* profiles = nullptr;
    ToolRunner* runner = nullptr;
    SocFlasher* flasher = nullptr;
    McuLink* link = nullptr;
    McuUpdater* updater = nullptr;
    ImagePuller* puller = nullptr;
    SshConnection* ssh = nullptr;
    SessionProvider sessionProvider;
    QString upgradeToolPath;
    QString adbPath;
    QString driverInstallerPath;
    bool toolPathsPinned = false;   ///< setToolPaths() was called: applyPreferences() leaves the paths alone
    QTimer statusTimer;
    QTimer linkCloseTimer;
    int refreshMs = kDefaultRefreshMs;

    // ---- Widgets (object names = the accessors) --------------------------------------------
    QGroupBox* sourceGroup = nullptr;
    QLabel* targetCaption = nullptr;
    QComboBox* sourceCombo = nullptr;
    QToolButton* profilesButton = nullptr;
    QLabel* remoteFolderCaption = nullptr;
    QLineEdit* remoteFolderEdit = nullptr;
    QLabel* cacheCaption = nullptr;
    QLineEdit* cacheDirEdit = nullptr;
    QToolButton* cacheDirButton = nullptr;
    QPushButton* pullButton = nullptr;
    QPushButton* localImageButton = nullptr;
    QLabel* imageCaption = nullptr;
    QLabel* imageInfoLabel = nullptr;
    QPushButton* rollbackButton = nullptr;

    QGroupBox* socGroup = nullptr;
    QLabel* deviceCaption = nullptr;
    QLabel* socStatusLabel = nullptr;
    QPushButton* installDriversButton = nullptr;
    QLabel* socImageCaption = nullptr;
    QLabel* socImageLabel = nullptr;
    QPushButton* flashSocButton = nullptr;
    QPushButton* cancelButton = nullptr;

    QGroupBox* mcuGroup = nullptr;
    QLabel* packageCaption = nullptr;
    QLineEdit* packageEdit = nullptr;
    QPushButton* packageButton = nullptr;
    QLabel* packageInfoLabel = nullptr;
    QLabel* runningCaption = nullptr;
    QLabel* mcuStatusLabel = nullptr;
    QPushButton* readMcuButton = nullptr;
    QLabel* roleCaption = nullptr;
    QRadioButton* roleRxRadio = nullptr;
    QRadioButton* roleTxRadio = nullptr;
    QButtonGroup* roleGroup = nullptr;
    QPushButton* updateMcuButton = nullptr;
    QPushButton* setRoleButton = nullptr;

    QPushButton* pullAndFlashButton = nullptr;
    QProgressBar* progressBar = nullptr;
    QLabel* progressLabel = nullptr;

    QGroupBox* logGroup = nullptr;
    QPlainTextEdit* logView = nullptr;
    QPushButton* clearLogButton = nullptr;
    QPushButton* saveLogButton = nullptr;

    // ---- Selection state ---------------------------------------------------------------------
    QString lastTargetKey;          ///< targetKey() the remote folder was last loaded for
    QString localImage;             ///< "Use local image..." pick; empty = the cache's update.img
    QString sfiImage;               ///< the image the SoC image line describes
    QString sfiText;                ///< its SFI summary (or why there is none)
    QPointer<ToolInvocation> sfiInvocation;
    bool hasPreviousImage = false;
    CcPackage mcuPackage;
    QString mcuPackagePath;
    QString packageProblem;         ///< why the last package was refused (shown while none is loaded)
    quint32 confirmedRole = 0;      ///< the role radios: 0 none, 1 rx, 2 tx
    QString runningVersion;
    QString runningRole;
    SocFlasher::DeviceStatus deviceStatus;
    bool hasDeviceStatus = false;
    QString boardInfoText;
    bool restoring = false;         ///< loading the remembered values: nothing is written back
    bool progressShowsReady = true; ///< the progress label still says "Ready" (retranslated with the rest)

    // ---- Operation state ---------------------------------------------------------------------
    QString operation;              ///< currentOperation(): the top-level operation
    QString step;                   ///< the step running right now (== operation unless Pull & Flash)
    QString persistent;             ///< persistentStatusMessage()
    bool cancelRequested = false;
    bool keepSsh = false;           ///< Pull & Flash keeps the build-machine connection until it ends
    PullPhase pullPhase = PullPhase::Idle;
    bool pullerActive = false;
    QString pullTarget;             ///< user@host:port of the running pull
    QString sshError;               ///< the connection's last errorOccurred() text
    bool flasherActive = false;
    bool flashCancellable = false;
    bool linkActive = false;
    bool updaterActive = false;
    bool mcuResultPending = false;  ///< waiting for McuLink::closed() before the step ends
    bool mcuOk = false;
    QString mcuMessage;
    QString linkLostMessage;
    quint32 mcuRole = 0;
    QPointer<ToolInvocation> cliInvocation;
    QString pendingVersion;         ///< "version" answered without a role: "help" decides it
    std::function<void(bool, const QString&)> cliDone;
    bool lastLogWasProgress = false;
    QString lastProgressStage;

    // ---- Construction ------------------------------------------------------------------------
    void setupUi();
    void wireCollaborators();
    void wireWidgets();
    void retranslate();
    void loadSettings();

    // ---- Source -----------------------------------------------------------------------------
    void rebuildTargets();
    QString selectedProfileId() const;
    QString targetKey() const;
    bool resolveProfile(SshProfile* profile, QString* error) const;
    void onTargetChanged(bool persist);
    void refreshImage(bool reread = false);   ///< reread: the file changed in place (pull, roll back)
    void runFirmwareInfo(const QString& image);
    void renderSocImage();

    // ---- Status / rendering ------------------------------------------------------------------
    void renderDeviceStatus();
    void renderPackage();
    void renderMcuStatus();
    void updateControls();
    bool cancellable() const;
    void updateStatusTimer();
    void pushToolPaths();
    void logToolPaths();

    // ---- Operations ----------------------------------------------------------------------------
    static QString operationTitle(const QString& op);
    bool refuseIfBusy();
    void beginOperation(const QString& op);
    void stepFinished(const QString& stepName, bool ok, const QString& message);
    void endOperation(const QString& stepName, bool ok, const QString& message);
    void setPersistent(const QString& text);
    void setProgressBusy(const QString& text);
    void setProgressPercent(int percent, const QString& text);

    void startPull();
    void startPuller();
    void pullFinished(bool ok, const QString& message);
    void onSshStateChanged(Transport::State state);
    void closeSsh();

    void startFlash();
    void flashFinished(bool ok, const QString& message);
    void onFlasherPhase(SocFlasher::Phase phase);
    void askDownloadMode();
    void showMaskromInstructions();

    void startMcuUpdate(const QString& stepName);
    void onLinkOpened();
    void mcuDone(bool ok, const QString& message);
    void completeMcu();

    void startMcuRead(std::function<void(bool, const QString&)> done);
    void runCli(const QString& command);
    void onCliFinished(const QString& command, const ToolResult& result);
    void finishCli(bool ok, const QString& message);
    void startChainMcu();

    void appendToolLine(const QString& line);
};

// =======================================================================================
// Construction
// =======================================================================================

FirmwareTab::FirmwareTab(SshProfileStore* profiles, ToolRunner* runner, QWidget* parent)
    : QWidget(parent)
    , d(std::make_unique<Private>(this))
{
    d->profiles = profiles;
    d->runner = runner ? runner : new ProcessToolRunner(this);
    d->flasher = new SocFlasher(d->runner, this);
    d->link = new McuLink(d->runner, this);
    d->updater = new McuUpdater(this);
    d->puller = new ImagePuller(this);
    d->ssh = new SshConnection(this);
    // The pull connection is short-lived and on demand: a dropped link ends the pull, it is not
    // re-established behind the user's back.
    d->ssh->setAutoReconnect(false);
    d->puller->setConnection(d->ssh);
    d->updater->setThroughSoc(true);   // the CLI "power" reply is logged before "bootloader"

    d->statusTimer.setInterval(d->refreshMs);
    connect(&d->statusTimer, &QTimer::timeout, this, &FirmwareTab::refreshStatus);
    d->linkCloseTimer.setSingleShot(true);
    connect(&d->linkCloseTimer, &QTimer::timeout, this, [this]() {
        if (d->mcuResultPending) {
            qCWarning(lcFirmware) << "MCU link did not report closed() in time";
            d->completeMcu();
        }
    });

    d->setupUi();
    d->wireWidgets();
    d->wireCollaborators();
    if (d->profiles) {
        connect(d->profiles, &SshProfileStore::changed, this, [this]() { d->rebuildTargets(); });
        connect(d->profiles, &QObject::destroyed, this, [this]() {
            d->profiles = nullptr;
            d->rebuildTargets();
        });
    }
    d->rebuildTargets();
    d->retranslate();
    d->loadSettings();
    applyPreferences();
    d->updateControls();
}

FirmwareTab::~FirmwareTab()
{
    // Children are destroyed after `d`: the line edits emit editingFinished when they lose the
    // focus on the way out and the collaborators may still report, so every connection into this
    // object is cut before anything is stopped.
    d->statusTimer.stop();
    d->linkCloseTimer.stop();
    const QList<QObject*> children = findChildren<QObject*>();
    for (QObject* child : children) {
        disconnect(child, nullptr, this, nullptr);
    }
    if (d->profiles) {
        disconnect(d->profiles, nullptr, this, nullptr);
    }
    for (ToolInvocation* invocation : {d->sfiInvocation.data(), d->cliInvocation.data()}) {
        if (invocation) {
            disconnect(invocation, nullptr, this, nullptr);
            invocation->cancel();
        }
    }
    if (d->puller->isRunning()) {
        d->puller->cancel();
    }
    if (d->updater->isRunning()) {
        d->updater->cancel();
    }
    if (d->flasher->isRunning()) {
        d->flasher->cancel();   // refused while upgrade_tool writes; MainWindow never closes a busy tab
    }
    d->link->close();
    d->ssh->close();
}

void FirmwareTab::Private::setupUi()
{
    auto* root = new QVBoxLayout(q);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    const auto makeButton = [this](const char* objectName) {
        auto* button = new QPushButton(q);
        button->setObjectName(QLatin1String(objectName));
        button->setAutoDefault(false);
        return button;
    };
    const auto makeRow = []() {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        return row;
    };
    const auto makeValueLabel = [this](const char* objectName) {
        auto* label = new QLabel(q);
        label->setObjectName(QLatin1String(objectName));
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // long texts never widen the tab
        return label;
    };

    // ---- Source ---------------------------------------------------------------------------
    sourceGroup = new QGroupBox(q);
    sourceGroup->setObjectName(QStringLiteral("sourceGroup"));
    auto* sourceLayout = new QVBoxLayout(sourceGroup);
    sourceLayout->setContentsMargins(6, 4, 6, 6);
    sourceLayout->setSpacing(4);

    targetCaption = new QLabel(q);
    sourceCombo = new QComboBox(q);
    sourceCombo->setObjectName(QStringLiteral("sourceCombo"));
    sourceCombo->setEditable(true);
    sourceCombo->setInsertPolicy(QComboBox::NoInsert);   // recent targets are the store's
    sourceCombo->setMinimumWidth(kTargetComboMinWidth);
    sourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sourceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    targetCaption->setBuddy(sourceCombo);
    profilesButton = new QToolButton(q);
    profilesButton->setObjectName(QStringLiteral("profilesButton"));
    profilesButton->setAutoRaise(true);
    profilesButton->setText(QStringLiteral("⚙"));   // gear, as in the SSH bar
    profilesButton->setFocusPolicy(Qt::NoFocus);
    remoteFolderCaption = new QLabel(q);
    remoteFolderEdit = new QLineEdit(q);
    remoteFolderEdit->setObjectName(QStringLiteral("remoteFolderEdit"));
    remoteFolderCaption->setBuddy(remoteFolderEdit);
    auto* targetRow = makeRow();
    targetRow->addWidget(targetCaption);
    targetRow->addWidget(sourceCombo, 2);
    targetRow->addWidget(profilesButton);
    targetRow->addSpacing(8);
    targetRow->addWidget(remoteFolderCaption);
    targetRow->addWidget(remoteFolderEdit, 3);
    sourceLayout->addLayout(targetRow);

    cacheCaption = new QLabel(q);
    cacheDirEdit = new QLineEdit(q);
    cacheDirEdit->setObjectName(QStringLiteral("cacheDirEdit"));
    cacheCaption->setBuddy(cacheDirEdit);
    cacheDirButton = new QToolButton(q);
    cacheDirButton->setObjectName(QStringLiteral("cacheDirButton"));
    cacheDirButton->setText(QStringLiteral("..."));
    pullButton = makeButton("pullButton");
    localImageButton = makeButton("localImageButton");
    auto* cacheRow = makeRow();
    cacheRow->addWidget(cacheCaption);
    cacheRow->addWidget(cacheDirEdit, 1);
    cacheRow->addWidget(cacheDirButton);
    cacheRow->addSpacing(8);
    cacheRow->addWidget(pullButton);
    cacheRow->addWidget(localImageButton);
    sourceLayout->addLayout(cacheRow);

    imageCaption = new QLabel(q);
    imageInfoLabel = makeValueLabel("imageInfoLabel");
    rollbackButton = makeButton("rollbackButton");
    auto* imageRow = makeRow();
    imageRow->addWidget(imageCaption);
    imageRow->addWidget(imageInfoLabel, 1);
    imageRow->addWidget(rollbackButton);
    sourceLayout->addLayout(imageRow);
    root->addWidget(sourceGroup);

    // ---- SoC ------------------------------------------------------------------------------
    socGroup = new QGroupBox(q);
    socGroup->setObjectName(QStringLiteral("socGroup"));
    auto* socLayout = new QVBoxLayout(socGroup);
    socLayout->setContentsMargins(6, 4, 6, 6);
    socLayout->setSpacing(4);

    deviceCaption = new QLabel(q);
    socStatusLabel = makeValueLabel("socStatusLabel");
    installDriversButton = makeButton("installDriversButton");
    installDriversButton->setVisible(false);   // only with the NoDriver status
    auto* deviceRow = makeRow();
    deviceRow->addWidget(deviceCaption);
    deviceRow->addWidget(socStatusLabel, 1);
    deviceRow->addWidget(installDriversButton);
    socLayout->addLayout(deviceRow);

    socImageCaption = new QLabel(q);
    socImageLabel = makeValueLabel("socImageLabel");
    auto* socImageRow = makeRow();
    socImageRow->addWidget(socImageCaption);
    socImageRow->addWidget(socImageLabel, 1);
    socLayout->addLayout(socImageRow);

    flashSocButton = makeButton("flashSocButton");
    cancelButton = makeButton("cancelButton");
    auto* flashRow = makeRow();
    flashRow->addWidget(flashSocButton);
    flashRow->addWidget(cancelButton);
    flashRow->addStretch(1);
    socLayout->addLayout(flashRow);
    root->addWidget(socGroup);

    // ---- MCU (CC1310) ---------------------------------------------------------------------
    mcuGroup = new QGroupBox(q);
    mcuGroup->setObjectName(QStringLiteral("mcuGroup"));
    auto* mcuLayout = new QVBoxLayout(mcuGroup);
    mcuLayout->setContentsMargins(6, 4, 6, 6);
    mcuLayout->setSpacing(4);

    packageCaption = new QLabel(q);
    packageEdit = new QLineEdit(q);
    packageEdit->setObjectName(QStringLiteral("packageEdit"));
    packageCaption->setBuddy(packageEdit);
    packageButton = makeButton("packageButton");
    packageButton->setText(QStringLiteral("..."));
    packageButton->setMaximumWidth(packageButton->fontMetrics().horizontalAdvance(QStringLiteral("...")) + 24);
    packageInfoLabel = makeValueLabel("packageInfoLabel");
    auto* packageRow = makeRow();
    packageRow->addWidget(packageCaption);
    packageRow->addWidget(packageEdit, 2);
    packageRow->addWidget(packageButton);
    packageRow->addSpacing(8);
    packageRow->addWidget(packageInfoLabel, 3);
    mcuLayout->addLayout(packageRow);

    runningCaption = new QLabel(q);
    mcuStatusLabel = makeValueLabel("mcuStatusLabel");
    mcuStatusLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    readMcuButton = makeButton("readMcuButton");
    roleCaption = new QLabel(q);
    roleRxRadio = new QRadioButton(QStringLiteral("rx"), q);
    roleRxRadio->setObjectName(QStringLiteral("roleRxRadio"));
    roleTxRadio = new QRadioButton(QStringLiteral("tx"), q);
    roleTxRadio->setObjectName(QStringLiteral("roleTxRadio"));
    roleGroup = new QButtonGroup(q);
    roleGroup->addButton(roleRxRadio, static_cast<int>(CcProtocol::RoleRx));
    roleGroup->addButton(roleTxRadio, static_cast<int>(CcProtocol::RoleTx));
    updateMcuButton = makeButton("updateMcuButton");
    setRoleButton = makeButton("setRoleButton");
    auto* roleRow = makeRow();
    roleRow->addWidget(runningCaption);
    roleRow->addWidget(mcuStatusLabel);
    roleRow->addWidget(readMcuButton);
    roleRow->addSpacing(16);
    roleRow->addWidget(roleCaption);
    roleRow->addWidget(roleRxRadio);
    roleRow->addWidget(roleTxRadio);
    roleRow->addSpacing(16);
    roleRow->addWidget(updateMcuButton);
    roleRow->addWidget(setRoleButton);
    roleRow->addStretch(1);
    mcuLayout->addLayout(roleRow);
    root->addWidget(mcuGroup);

    // ---- Pull & Flash -----------------------------------------------------------------------
    pullAndFlashButton = makeButton("pullAndFlashButton");
    QFont bold = pullAndFlashButton->font();
    bold.setBold(true);
    pullAndFlashButton->setFont(bold);
    progressBar = new QProgressBar(q);
    progressBar->setObjectName(QStringLiteral("progressBar"));
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressLabel = makeValueLabel("progressLabel");
    auto* actionRow = makeRow();
    actionRow->setContentsMargins(2, 0, 2, 0);
    actionRow->addWidget(pullAndFlashButton);
    actionRow->addWidget(progressBar, 2);
    actionRow->addWidget(progressLabel, 3);
    root->addLayout(actionRow);

    // ---- Log --------------------------------------------------------------------------------
    logGroup = new QGroupBox(q);
    logGroup->setObjectName(QStringLiteral("logGroup"));
    auto* logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(6, 4, 6, 6);
    logLayout->setSpacing(4);
    logView = new QPlainTextEdit(q);
    logView->setObjectName(QStringLiteral("logView"));
    logView->setReadOnly(true);
    logView->setUndoRedoEnabled(false);
    logView->setMaximumBlockCount(kLogMaxBlocks);
    logView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    logView->setFont(logFont());
    logLayout->addWidget(logView, 1);
    clearLogButton = makeButton("clearLogButton");
    saveLogButton = makeButton("saveLogButton");
    auto* logButtons = makeRow();
    logButtons->addStretch(1);
    logButtons->addWidget(clearLogButton);
    logButtons->addWidget(saveLogButton);
    logLayout->addLayout(logButtons);
    root->addWidget(logGroup, 1);
}

void FirmwareTab::Private::wireWidgets()
{
    // ---- Source ----
    connect(sourceCombo, &QComboBox::activated, q, [this](int) { onTargetChanged(true); });
    if (QLineEdit* edit = sourceCombo->lineEdit()) {
        connect(edit, &QLineEdit::editingFinished, q, [this]() { onTargetChanged(true); });
    }
    connect(profilesButton, &QToolButton::clicked, q, &FirmwareTab::sshProfilesEditRequested);
    connect(remoteFolderEdit, &QLineEdit::editingFinished, q, [this]() {
        const QString folder = remoteFolderEdit->text().trimmed();
        if (!folder.isEmpty() && folder != AppSettings::instance().firmwareRemoteFolder(targetKey())) {
            AppSettings::instance().setFirmwareRemoteFolder(targetKey(), folder);
        }
    });
    connect(cacheDirEdit, &QLineEdit::editingFinished, q, [this]() {
        const QString dir = QDir::fromNativeSeparators(cacheDirEdit->text().trimmed());
        if (dir != AppSettings::instance().firmwareCacheDirectory()) {
            q->setCacheDirectory(dir);
        }
    });
    connect(cacheDirButton, &QToolButton::clicked, q, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(q, FirmwareTab::tr("Choose the Local Image Cache"), q->cacheDirectory(),
                                                              QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!dir.isEmpty()) {
            q->setCacheDirectory(dir);
        }
    });
    connect(pullButton, &QPushButton::clicked, q, &FirmwareTab::pull);
    connect(localImageButton, &QPushButton::clicked, q, &FirmwareTab::chooseLocalImage);
    connect(rollbackButton, &QPushButton::clicked, q, &FirmwareTab::rollBack);

    // ---- SoC ----
    connect(flashSocButton, &QPushButton::clicked, q, &FirmwareTab::flashSoc);
    connect(cancelButton, &QPushButton::clicked, q, &FirmwareTab::cancel);
    connect(installDriversButton, &QPushButton::clicked, q, &FirmwareTab::installDrivers);

    // ---- MCU ----
    connect(packageEdit, &QLineEdit::editingFinished, q, [this]() {
        const QString typed = QDir::fromNativeSeparators(packageEdit->text().trimmed());
        if (typed != mcuPackagePath) {
            q->setPackagePath(typed);
        }
    });
    connect(packageButton, &QPushButton::clicked, q, &FirmwareTab::choosePackage);
    connect(readMcuButton, &QPushButton::clicked, q, &FirmwareTab::readMcuInfo);
    connect(roleRxRadio, &QRadioButton::clicked, q, [this]() { q->setSelectedRole(CcProtocol::RoleRx); });
    connect(roleTxRadio, &QRadioButton::clicked, q, [this]() { q->setSelectedRole(CcProtocol::RoleTx); });
    connect(updateMcuButton, &QPushButton::clicked, q, &FirmwareTab::updateMcu);
    connect(setRoleButton, &QPushButton::clicked, q, &FirmwareTab::setMcuRole);

    // ---- Pull & Flash, log ----
    connect(pullAndFlashButton, &QPushButton::clicked, q, &FirmwareTab::pullAndFlash);
    connect(clearLogButton, &QPushButton::clicked, q, &FirmwareTab::clearLog);
    connect(saveLogButton, &QPushButton::clicked, q, &FirmwareTab::saveLog);
}

void FirmwareTab::Private::wireCollaborators()
{
    // ---- Build machine connection (the same prompts as a session tab) ----
    connect(ssh, &Transport::stateChanged, q, [this](Transport::State state) { onSshStateChanged(state); });
    connect(ssh, &Transport::errorOccurred, q, [this](const QString& message) {
        sshError = message;
        q->appendLog(FirmwareTab::tr("SSH: %1").arg(message));
    });
    connect(ssh, &SshConnection::authenticated, q, [this](const QString& method) {
        q->appendLog(FirmwareTab::tr("Authenticated to %1 (%2)").arg(pullTarget, method));
    });
    connect(ssh, &SshConnection::hostKeyVerificationRequired, q, [this](const SshConnection::HostKeyInfo& info) {
        // As SessionWidget::onHostKeyVerificationRequired(): the dialog lives on the heap and the
        // tab is tracked, because either may go away during the nested event loop.
        QPointer<FirmwareTab> self(q);
        QPointer<HostKeyDialog> dialog(new HostKeyDialog(info, q->window()));
        const int result = dialog->exec();
        if (!self || !dialog) {
            delete dialog;
            return;
        }
        const bool accepted = (result == QDialog::Accepted);
        const bool remember = accepted && dialog->remember();
        delete dialog;
        qCInfo(lcFirmware) << "host key" << info.keyType << info.fingerprintSha256
                           << (accepted ? "accepted" : "rejected") << (remember ? "(remembered)" : "");
        ssh->answerHostKey(accepted, remember);
    });
    connect(ssh, &SshConnection::authPromptRequired, q, [this](const SshConnection::AuthPrompt& prompt) {
        QPointer<FirmwareTab> self(q);
        QPointer<AuthPromptDialog> dialog(new AuthPromptDialog(prompt, q->window()));
        const int result = dialog->exec();
        if (!self || !dialog) {
            delete dialog;
            return;
        }
        if (result == QDialog::Accepted) {
            const QString response = dialog->response();
            const bool remember = dialog->remember();
            delete dialog;
            ssh->answerPrompt(response, remember);
        } else {
            delete dialog;
            qCInfo(lcFirmware) << "authentication prompt cancelled";
            ssh->cancelPrompt();
        }
    });

    // ---- Image pull ----
    connect(puller, &ImagePuller::remoteInfo, q,
            [this](const QString& name, qint64 size, const QDateTime& modified, const QString& describe) {
                QString text = FirmwareTab::tr("Remote %1: %2, built %3").arg(name, formatSize(size), formatTime(modified));
                if (!describe.isEmpty()) {
                    text += QStringLiteral(" (%1)").arg(describe);
                }
                q->appendLog(text);
                progressLabel->setText(text);
            });
    connect(puller, &ImagePuller::progress, q, [this](qint64 done, qint64 total) {
        if (!pullerActive) {
            return;
        }
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : -1;
        const QString text = total > 0 ? FirmwareTab::tr("Pulling update.img: %1 / %2 (%3 %)")
                                             .arg(formatSize(done), formatSize(total))
                                             .arg(percent)
                                       : FirmwareTab::tr("Pulling update.img: %1").arg(formatSize(done));
        if (percent >= 0) {
            setProgressPercent(percent, text);
        } else {
            setProgressBusy(text);
        }
        setPersistent(text);
    });
    connect(puller, &ImagePuller::skipped, q, [this](const QString& reason) { q->appendLog(reason); });
    connect(puller, &ImagePuller::logLine, q, [this](const QString& line) { q->appendLog(line); });
    connect(puller, &ImagePuller::finished, q, [this](bool ok, const QString& message) {
        if (pullerActive) {
            pullFinished(ok, message);
        }
    });

    // ---- SoC flasher ----
    connect(flasher, &SocFlasher::statusChanged, q, [this](const SocFlasher::DeviceStatus& status) {
        deviceStatus = status;
        hasDeviceStatus = true;
        renderDeviceStatus();
    });
    connect(flasher, &SocFlasher::phaseChanged, q, [this](SocFlasher::Phase phase) { onFlasherPhase(phase); });
    connect(flasher, &SocFlasher::downloadModeNeeded, q, [this]() {
        // Not inside the flasher's emit: the question runs a nested event loop.
        QMetaObject::invokeMethod(q, [this]() { askDownloadMode(); }, Qt::QueuedConnection);
    });
    connect(flasher, &SocFlasher::progress, q, [this](const QString& stage, int percent) {
        if (!flasherActive) {
            return;
        }
        const QString text = percent >= 0 ? FirmwareTab::tr("Flashing SoC: %1 %2 %").arg(stage).arg(percent)
                                          : FirmwareTab::tr("Flashing SoC: %1").arg(stage);
        if (percent >= 0) {
            setProgressPercent(percent, text);
        } else {
            progressLabel->setText(text);
        }
        setPersistent(text);
    });
    connect(flasher, &SocFlasher::toolOutput, q, [this](const QString& line) { appendToolLine(line); });
    connect(flasher, &SocFlasher::logLine, q, [this](const QString& line) { q->appendLog(line); });
    connect(flasher, &SocFlasher::boardInfo, q, [this](const QString& text) {
        boardInfoText = text.trimmed();
        q->appendLog(FirmwareTab::tr("Board information:"));
        q->appendLog(boardInfoText);
        socStatusLabel->setToolTip(boardInfoText);
    });
    connect(flasher, &SocFlasher::finished, q, [this](bool ok, const QString& message) {
        if (flasherActive) {
            flashFinished(ok, message);
        } else {
            q->appendLog(FirmwareTab::tr("SoC flasher: %1").arg(message));   // a late report of a step already ended
        }
    });

    // ---- MCU link / updater ----
    connect(link, &McuLink::serialSelected, q,
            [this](const QString& serial) { q->appendLog(FirmwareTab::tr("MCU link: board %1").arg(serial)); });
    connect(link, &McuLink::opened, q, [this](const QString&, quint16) { onLinkOpened(); });
    connect(link, &McuLink::failed, q, [this](const QString& message) {
        if (linkActive && !updaterActive && !mcuResultPending) {
            linkActive = false;
            stepFinished(step, false, message);
        }
    });
    connect(link, &McuLink::linkLost, q, [this](const QString& message) {
        linkLostMessage = message;
        q->appendLog(message);
        if (updaterActive && !updater->isRunning()) {
            updaterActive = false;
            mcuDone(false, message);
        }
    });
    connect(link, &McuLink::closed, q, [this]() {
        if (mcuResultPending) {
            completeMcu();
        }
    });
    connect(link, &McuLink::logLine, q, [this](const QString& line) { q->appendLog(line); });

    connect(updater, &McuUpdater::logLine, q, [this](const QString& line) { q->appendLog(line); });
    connect(updater, &McuUpdater::phaseChanged, q, [this](McuUpdater::Phase phase) {
        if (!updaterActive) {
            return;
        }
        QString text;
        switch (phase) {
        case McuUpdater::Phase::ProbeBootloader:
        case McuUpdater::Phase::ProbeApplication:
            text = FirmwareTab::tr("MCU: probing the bootloader and the application");
            break;
        case McuUpdater::Phase::EnterBootloader:
        case McuUpdater::Phase::SettleAfterReset:
        case McuUpdater::Phase::RebootProbe:
            text = FirmwareTab::tr("MCU: rebooting into the UART updater");
            break;
        case McuUpdater::Phase::Banner:
            text = FirmwareTab::tr("MCU: waiting for the application to restart");
            break;
        default:
            break;
        }
        if (!text.isEmpty()) {
            progressLabel->setText(text);
        }
    });
    connect(updater, &McuUpdater::progress, q, [this](qint64 done, qint64 total) {
        if (!updaterActive) {
            return;
        }
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : 0;
        const QString text = FirmwareTab::tr("Updating MCU: %1 / %2 bytes (%3 %)")
                                 .arg(QLocale().toString(done), QLocale().toString(total))
                                 .arg(percent);
        setProgressPercent(percent, text);
        setPersistent(text);
    });
    connect(updater, &McuUpdater::applicationInfo, q, [this](const McuUpdater::CliFields& fields) {
        if (!fields.value(QStringLiteral("version")).isEmpty()) {
            runningVersion = fields.value(QStringLiteral("version"));
            runningRole = fields.value(QStringLiteral("role"));
            renderMcuStatus();
        }
    });
    connect(updater, &McuUpdater::applicationRestarted, q, [this](const McuUpdater::CliFields& fields) {
        if (!fields.value(QStringLiteral("version")).isEmpty()) {
            runningVersion = fields.value(QStringLiteral("version"));
        }
        if (!fields.value(QStringLiteral("role")).isEmpty()) {
            runningRole = fields.value(QStringLiteral("role"));
        }
        renderMcuStatus();
    });
    connect(updater, &McuUpdater::finished, q, [this](bool ok, const QString& message) {
        if (!updaterActive) {
            return;
        }
        updaterActive = false;
        mcuDone(ok, (!ok && !linkLostMessage.isEmpty()) ? linkLostMessage : message);
    });
}

void FirmwareTab::Private::retranslate()
{
    sourceGroup->setTitle(FirmwareTab::tr("Source"));
    targetCaption->setText(FirmwareTab::tr("Build machine:"));
    sourceCombo->setToolTip(FirmwareTab::tr("SSH profile or user@host[:port] of the Linux build machine"));
    if (QLineEdit* edit = sourceCombo->lineEdit()) {
        edit->setPlaceholderText(FirmwareTab::tr("user@host[:port]"));
    }
    profilesButton->setToolTip(FirmwareTab::tr("Manage SSH profiles..."));
    remoteFolderCaption->setText(FirmwareTab::tr("Remote folder:"));
    remoteFolderEdit->setToolTip(FirmwareTab::tr("Folder on the build machine that holds update.img (\"~\" is the remote home)"));
    remoteFolderEdit->setPlaceholderText(AppSettings::defaultFirmwareRemoteFolder());
    cacheCaption->setText(FirmwareTab::tr("Local cache:"));
    cacheDirEdit->setToolTip(FirmwareTab::tr("Folder on this PC that keeps the pulled image and the previous one"));
    cacheDirButton->setToolTip(FirmwareTab::tr("Choose the cache folder"));
    pullButton->setText(FirmwareTab::tr("Pull"));
    pullButton->setToolTip(FirmwareTab::tr("Copy update.img from the build machine (skipped when it did not change)"));
    localImageButton->setText(FirmwareTab::tr("Use local image..."));
    localImageButton->setToolTip(FirmwareTab::tr("Flash an update.img from this PC instead of the build machine"));
    imageCaption->setText(FirmwareTab::tr("Image:"));
    rollbackButton->setText(FirmwareTab::tr("Roll back"));
    rollbackButton->setToolTip(FirmwareTab::tr("Swap the cached image with the previous one"));

    socGroup->setTitle(FirmwareTab::tr("SoC"));
    deviceCaption->setText(FirmwareTab::tr("Device:"));
    installDriversButton->setText(FirmwareTab::tr("Install drivers"));
    installDriversButton->setToolTip(FirmwareTab::tr("Run the bundled Rockchip DriverAssistant (rockusb and ADB drivers)"));
    socImageCaption->setText(FirmwareTab::tr("Image:"));
    flashSocButton->setText(FirmwareTab::tr("Flash SoC"));
    flashSocButton->setToolTip(FirmwareTab::tr("Write the image to the Rockchip SoC over the USB-C cable (upgrade_tool UF)"));
    cancelButton->setText(FirmwareTab::tr("Cancel"));
    cancelButton->setToolTip(FirmwareTab::tr("Stop the running step (not while upgrade_tool writes the flash)"));

    mcuGroup->setTitle(FirmwareTab::tr("MCU (CC1310)"));
    packageCaption->setText(FirmwareTab::tr("Package:"));
    packageEdit->setToolTip(FirmwareTab::tr("CC1310 firmware package (.pkg)"));
    packageButton->setToolTip(FirmwareTab::tr("Choose the package"));
    runningCaption->setText(FirmwareTab::tr("Running:"));
    readMcuButton->setText(FirmwareTab::tr("Read"));
    readMcuButton->setToolTip(FirmwareTab::tr("Ask the MCU for its version and role (adb shell rfmcu cmd version)"));
    roleCaption->setText(FirmwareTab::tr("Role:"));
    roleRxRadio->setToolTip(FirmwareTab::tr("Receiver firmware role"));
    roleTxRadio->setToolTip(FirmwareTab::tr("Transmitter firmware role"));
    updateMcuButton->setText(FirmwareTab::tr("Update MCU"));
    updateMcuButton->setToolTip(FirmwareTab::tr("Send the package to the CC1310 through the board (needs a confirmed role)"));
    setRoleButton->setText(FirmwareTab::tr("Set role"));
    setRoleButton->setToolTip(FirmwareTab::tr("Store the selected role in the CC1310"));

    pullAndFlashButton->setText(FirmwareTab::tr("Pull && Flash"));
    pullAndFlashButton->setToolTip(
        FirmwareTab::tr("Pull the image, flash the SoC, wait for the board, then update the MCU when its version differs"));
    logGroup->setTitle(FirmwareTab::tr("Log"));
    clearLogButton->setText(FirmwareTab::tr("Clear"));
    clearLogButton->setToolTip(FirmwareTab::tr("Empty the log pane"));
    saveLogButton->setText(FirmwareTab::tr("Save..."));
    saveLogButton->setToolTip(FirmwareTab::tr("Save the log pane to a text file"));

    if (operation.isEmpty() && progressShowsReady) {
        progressLabel->setText(FirmwareTab::tr("Ready"));
    }
    refreshImage();
    renderSocImage();
    renderDeviceStatus();
    renderPackage();
    renderMcuStatus();
}

void FirmwareTab::Private::loadSettings()
{
    const AppSettings& s = AppSettings::instance();
    restoring = true;
    cacheDirEdit->setText(nativePath(s.firmwareCacheDirectory()));
    q->setSourceTarget(s.firmwareSourceTarget());   // also loads that target's remote folder

    const QString local = s.firmwareLocalImage();
    if (!local.isEmpty() && QFileInfo(local).isFile() && RockchipTool::isRkfwImage(local)) {
        localImage = local;
    }
    const QString packageFile = s.firmwarePackagePath();
    if (!packageFile.isEmpty() && QFileInfo(packageFile).isFile()) {
        CcPackage loaded;
        QString error;
        if (CcPackage::load(packageFile, &loaded, &error)) {
            mcuPackage = loaded;
            mcuPackagePath = packageFile;
        } else {
            qCInfo(lcFirmware) << "remembered package" << packageFile << "not loaded:" << error;
        }
    }
    packageEdit->setText(nativePath(mcuPackagePath));
    q->setSelectedRole(roleFromText(QSettings().value(kRoleSettingsKey).toString()));
    restoring = false;

    refreshImage();
    renderPackage();
}

// =======================================================================================
// Accessors
// =======================================================================================

QString FirmwareTab::title() const
{
    return tr("Firmware");
}

bool FirmwareTab::isBusy() const
{
    return !d->operation.isEmpty();
}

QString FirmwareTab::persistentStatusMessage() const
{
    return isBusy() ? d->persistent : QString();
}

QString FirmwareTab::currentOperation() const
{
    return d->operation;
}

ToolRunner* FirmwareTab::toolRunner() const { return d->runner; }
SocFlasher* FirmwareTab::socFlasher() const { return d->flasher; }
McuLink* FirmwareTab::mcuLink() const { return d->link; }
McuUpdater* FirmwareTab::mcuUpdater() const { return d->updater; }
ImagePuller* FirmwareTab::imagePuller() const { return d->puller; }
SshConnection* FirmwareTab::sshConnection() const { return d->ssh; }

void FirmwareTab::setToolPaths(const QString& upgradeTool, const QString& adb, const QString& driverInstaller)
{
    d->upgradeToolPath = upgradeTool;
    d->adbPath = adb;
    d->driverInstallerPath = driverInstaller;
    d->toolPathsPinned = true;
    d->pushToolPaths();
    qCInfo(lcFirmware) << "tool paths set: upgrade_tool" << upgradeTool << "adb" << adb;
    d->runFirmwareInfo(imagePath());   // the image line depends on upgrade_tool
}

void FirmwareTab::setSessionProvider(SessionProvider provider)
{
    d->sessionProvider = std::move(provider);
}

void FirmwareTab::setStatusRefreshIntervalMs(int ms)
{
    d->refreshMs = qMax(0, ms);
    d->updateStatusTimer();
}

int FirmwareTab::statusRefreshIntervalMs() const
{
    return d->refreshMs;
}

QString FirmwareTab::sourceTarget() const
{
    return d->sourceCombo->currentText().trimmed();
}

void FirmwareTab::setSourceTarget(const QString& keyOrTarget)
{
    const QString text = keyOrTarget.trimmed();
    {
        const QSignalBlocker blocker(d->sourceCombo);
        int index = -1;
        QString shown = text;
        if (text.startsWith(kProfileKeyPrefix)) {
            index = d->sourceCombo->findData(text.mid(kProfileKeyPrefix.size()));
        } else {
            if (text.startsWith(kTargetKeyPrefix)) {
                shown = text.mid(kTargetKeyPrefix.size());
            }
            index = shown.isEmpty() ? -1 : d->sourceCombo->findText(shown, Qt::MatchFixedString | Qt::MatchCaseSensitive);
        }
        if (index >= 0 && !isSeparator(d->sourceCombo, index)) {
            d->sourceCombo->setCurrentIndex(index);
        } else {
            d->sourceCombo->setCurrentIndex(-1);
            d->sourceCombo->setEditText(shown);   // free text; an unknown profile key stays visible, Pull explains it
        }
    }
    d->onTargetChanged(!d->restoring);
}

QString FirmwareTab::remoteFolder() const
{
    return d->remoteFolderEdit->text().trimmed();
}

void FirmwareTab::setRemoteFolder(const QString& folder)
{
    const QString trimmed = folder.trimmed();
    d->remoteFolderEdit->setText(trimmed);
    if (!d->restoring && !trimmed.isEmpty()) {
        AppSettings::instance().setFirmwareRemoteFolder(d->targetKey(), trimmed);
    }
}

QString FirmwareTab::cacheDirectory() const
{
    return QDir::fromNativeSeparators(d->cacheDirEdit->text().trimmed());
}

void FirmwareTab::setCacheDirectory(const QString& dir)
{
    const QString clean = QDir::fromNativeSeparators(dir.trimmed());
    d->cacheDirEdit->setText(nativePath(clean));
    if (!d->restoring) {
        AppSettings::instance().setFirmwareCacheDirectory(clean);
    }
    d->refreshImage();
}

QString FirmwareTab::imagePath() const
{
    if (!d->localImage.isEmpty()) {
        return d->localImage;
    }
    const QString cache = cacheDirectory();
    if (cache.isEmpty()) {
        return QString();
    }
    const QString cached = ImagePuller::imagePath(cache);
    return QFileInfo(cached).isFile() ? cached : QString();
}

void FirmwareTab::setImagePath(const QString& path)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    if (clean.isEmpty()) {
        d->localImage.clear();
        AppSettings::instance().setFirmwareLocalImage(QString());
        d->refreshImage();
        return;
    }
    QString error;
    if (!QFileInfo(clean).isFile()) {
        error = tr("file not found");
    } else if (!RockchipTool::isRkfwImage(clean, &error)) {
        if (error.isEmpty()) {
            error = tr("not a Rockchip RKFW image");
        }
    }
    if (!error.isEmpty()) {
        const QString message = tr("Image %1 refused: %2").arg(nativePath(clean), error);
        appendLog(message);
        emit statusMessage(message, kStatusLongMs);
        return;
    }
    d->localImage = clean;
    AppSettings::instance().setFirmwareLocalImage(clean);
    appendLog(tr("Using the local image %1").arg(nativePath(clean)));
    d->refreshImage();
}

QString FirmwareTab::packagePath() const
{
    return d->mcuPackagePath;
}

bool FirmwareTab::setPackagePath(const QString& path)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    if (clean.isEmpty()) {
        d->mcuPackage = CcPackage();
        d->mcuPackagePath.clear();
        d->packageProblem.clear();
        AppSettings::instance().setFirmwarePackagePath(QString());
        d->packageEdit->clear();
        d->renderPackage();
        d->updateControls();
        return false;
    }
    CcPackage loaded;
    QString error;
    if (!CcPackage::load(clean, &loaded, &error)) {
        if (error.isEmpty()) {
            error = tr("unreadable file");
        }
        const QString message = tr("Package %1 refused: %2").arg(QFileInfo(clean).fileName(), error);
        appendLog(message);
        emit statusMessage(message, kStatusLongMs);
        d->packageProblem = message;
        d->packageEdit->setText(nativePath(d->mcuPackagePath));   // back to the accepted one
        d->renderPackage();
        return false;
    }
    d->mcuPackage = loaded;
    d->mcuPackagePath = clean;
    d->packageProblem.clear();
    AppSettings::instance().setFirmwarePackagePath(clean);
    d->packageEdit->setText(nativePath(clean));
    const QString summary = loaded.summary();
    appendLog(tr("Package %1: %2").arg(QFileInfo(clean).fileName(),
                                        summary.isEmpty() ? d->packageInfoLabel->text() : summary));
    d->renderPackage();
    d->updateControls();
    return true;
}

CcPackage FirmwareTab::package() const
{
    return d->mcuPackage;
}

quint32 FirmwareTab::selectedRole() const
{
    return d->confirmedRole;
}

void FirmwareTab::setSelectedRole(quint32 role)
{
    const quint32 valid = (role == CcProtocol::RoleRx || role == CcProtocol::RoleTx) ? role : 0;
    d->confirmedRole = valid;
    {
        const QSignalBlocker rxBlocker(d->roleRxRadio);
        const QSignalBlocker txBlocker(d->roleTxRadio);
        d->roleGroup->setExclusive(false);   // an exclusive group cannot show "none chosen"
        d->roleRxRadio->setChecked(valid == CcProtocol::RoleRx);
        d->roleTxRadio->setChecked(valid == CcProtocol::RoleTx);
        d->roleGroup->setExclusive(true);
    }
    if (!d->restoring) {
        QSettings().setValue(kRoleSettingsKey, roleText(valid));
    }
    d->updateControls();
}

QString FirmwareTab::runningMcuVersion() const
{
    return d->runningVersion;
}

QString FirmwareTab::runningMcuRole() const
{
    return d->runningRole;
}

QComboBox* FirmwareTab::sourceCombo() const { return d->sourceCombo; }
QLineEdit* FirmwareTab::remoteFolderEdit() const { return d->remoteFolderEdit; }
QLineEdit* FirmwareTab::cacheDirEdit() const { return d->cacheDirEdit; }
QPushButton* FirmwareTab::pullButton() const { return d->pullButton; }
QPushButton* FirmwareTab::localImageButton() const { return d->localImageButton; }
QLabel* FirmwareTab::imageInfoLabel() const { return d->imageInfoLabel; }
QPushButton* FirmwareTab::rollbackButton() const { return d->rollbackButton; }
QLabel* FirmwareTab::socStatusLabel() const { return d->socStatusLabel; }
QLabel* FirmwareTab::socImageLabel() const { return d->socImageLabel; }
QPushButton* FirmwareTab::flashSocButton() const { return d->flashSocButton; }
QPushButton* FirmwareTab::cancelButton() const { return d->cancelButton; }
QLineEdit* FirmwareTab::packageEdit() const { return d->packageEdit; }
QPushButton* FirmwareTab::packageButton() const { return d->packageButton; }
QLabel* FirmwareTab::packageInfoLabel() const { return d->packageInfoLabel; }
QLabel* FirmwareTab::mcuStatusLabel() const { return d->mcuStatusLabel; }
QPushButton* FirmwareTab::readMcuButton() const { return d->readMcuButton; }
QRadioButton* FirmwareTab::roleRxRadio() const { return d->roleRxRadio; }
QRadioButton* FirmwareTab::roleTxRadio() const { return d->roleTxRadio; }
QPushButton* FirmwareTab::updateMcuButton() const { return d->updateMcuButton; }
QPushButton* FirmwareTab::setRoleButton() const { return d->setRoleButton; }
QPushButton* FirmwareTab::pullAndFlashButton() const { return d->pullAndFlashButton; }
QProgressBar* FirmwareTab::progressBar() const { return d->progressBar; }
QLabel* FirmwareTab::progressLabel() const { return d->progressLabel; }
QPlainTextEdit* FirmwareTab::logView() const { return d->logView; }
QPushButton* FirmwareTab::clearLogButton() const { return d->clearLogButton; }
QPushButton* FirmwareTab::saveLogButton() const { return d->saveLogButton; }
QPushButton* FirmwareTab::installDriversButton() const { return d->installDriversButton; }

// =======================================================================================
// Source
// =======================================================================================

void FirmwareTab::Private::rebuildTargets()
{
    // Same items as SshConnectionBar::rebuild(): stored profiles (item data = id), a separator,
    // the recent ad-hoc targets; what is shown now survives the rebuild.
    const QString beforeId = selectedProfileId();
    const QString beforeText = sourceCombo->currentText().trimmed();
    const QSignalBlocker blocker(sourceCombo);
    sourceCombo->clear();
    QList<SshProfile> stored;
    QStringList recent;
    if (profiles) {
        stored = profiles->profiles();
        recent = profiles->recentTargets();
    }
    for (const SshProfile& profile : std::as_const(stored)) {
        sourceCombo->addItem(profileItemText(profile), profile.id);
        sourceCombo->setItemData(sourceCombo->count() - 1, SshProfile::authText(profile.auth), Qt::ToolTipRole);
    }
    if (!stored.isEmpty() && !recent.isEmpty()) {
        sourceCombo->insertSeparator(sourceCombo->count());
    }
    for (const QString& target : std::as_const(recent)) {
        sourceCombo->addItem(target);
    }
    sourceCombo->setCurrentIndex(-1);
    const int byId = beforeId.isEmpty() ? -1 : sourceCombo->findData(beforeId);
    if (byId >= 0) {
        sourceCombo->setCurrentIndex(byId);
    } else if (!beforeText.isEmpty()) {
        const int recentIndex = findRecentItem(sourceCombo, beforeText);
        if (recentIndex >= 0) {
            sourceCombo->setCurrentIndex(recentIndex);
        } else {
            sourceCombo->setEditText(beforeText);
        }
    }
}

QString FirmwareTab::Private::selectedProfileId() const
{
    const int index = sourceCombo->currentIndex();
    if (index < 0 || isSeparator(sourceCombo, index)) {
        return QString();
    }
    const QString id = sourceCombo->itemData(index).toString();
    if (id.isEmpty() || sourceCombo->itemText(index) != sourceCombo->currentText().trimmed()) {
        return QString();   // edited after the pick: the text rules
    }
    return id;
}

QString FirmwareTab::Private::targetKey() const
{
    const QString id = selectedProfileId();
    return id.isEmpty() ? sourceCombo->currentText().trimmed() : QString(kProfileKeyPrefix) + id;
}

bool FirmwareTab::Private::resolveProfile(SshProfile* profile, QString* error) const
{
    QString text = sourceCombo->currentText().trimmed();
    QString id = selectedProfileId();
    if (id.isEmpty() && text.startsWith(kProfileKeyPrefix)) {
        id = text.mid(kProfileKeyPrefix.size());
    }
    if (!id.isEmpty()) {
        const std::optional<SshProfile> stored = profiles ? profiles->profile(id) : std::nullopt;
        if (stored) {
            *profile = *stored;
            return true;
        }
        *error = FirmwareTab::tr("the SSH profile of the build machine no longer exists");
        return false;
    }
    if (text.isEmpty()) {
        *error = FirmwareTab::tr("enter the build machine (an SSH profile or user@host)");
        return false;
    }
    if (text.startsWith(kTargetKeyPrefix)) {
        text = text.mid(kTargetKeyPrefix.size());
    }
    if (profiles) {
        if (const std::optional<SshProfile> byName = profiles->profileByName(text)) {
            *profile = *byName;
            return true;
        }
    }
    SshProfile adHoc;
    if (!SshProfile::parseTarget(text, adHoc)) {
        *error = FirmwareTab::tr("\"%1\" is not an SSH target such as user@host[:port]").arg(text);
        return false;
    }
    *profile = adHoc;
    return true;
}

void FirmwareTab::Private::onTargetChanged(bool persist)
{
    const QString key = targetKey();
    if (key != lastTargetKey || !persist) {
        lastTargetKey = key;
        // A target remembers its own folder; a new one starts with the last folder used anywhere.
        remoteFolderEdit->setText(AppSettings::instance().firmwareRemoteFolder(key));
    }
    if (persist && key != AppSettings::instance().firmwareSourceTarget()) {
        AppSettings::instance().setFirmwareSourceTarget(key);
    }
    if (operation.isEmpty() && ssh->state() != Transport::State::Disconnected) {
        closeSsh();   // a connection to the previous target is of no use any more
    }
}

void FirmwareTab::Private::refreshImage(bool reread)
{
    if (reread) {
        sfiImage.clear();   // same path, new content: describe it again
    }
    const QString image = q->imagePath();
    const QString cache = q->cacheDirectory();
    QString text;
    if (image.isEmpty()) {
        text = FirmwareTab::tr("no image yet: pull one from the build machine or use a local image");
    } else {
        const QFileInfo info(image);
        if (!localImage.isEmpty()) {
            text = FirmwareTab::tr("%1, %2, modified %3 (local file)")
                       .arg(nativePath(image), formatSize(info.size()), formatTime(info.lastModified()));
        } else {
            const ImagePuller::CacheEntry entry = ImagePuller::cachedImage(cache);
            const QString summary = entry.isValid() ? entry.summary() : QString();
            text = summary.isEmpty() ? FirmwareTab::tr("%1, %2, modified %3")
                                           .arg(info.fileName(), formatSize(info.size()), formatTime(info.lastModified()))
                                     : summary;
        }
    }
    const QString previousPath = cache.isEmpty() ? QString() : ImagePuller::previousImagePath(cache);
    hasPreviousImage = !previousPath.isEmpty() && QFileInfo(previousPath).isFile();
    if (hasPreviousImage) {
        const ImagePuller::CacheEntry previous = ImagePuller::previousImage(cache);
        QString summary = previous.isValid() ? previous.summary() : QString();
        if (summary.isEmpty()) {
            const QFileInfo info(previousPath);
            summary = FirmwareTab::tr("%1, %2, modified %3").arg(info.fileName(), formatSize(info.size()), formatTime(info.lastModified()));
        }
        text += QStringLiteral(" · ") + FirmwareTab::tr("previous: %1").arg(summary);
    }
    imageInfoLabel->setText(text);
    imageInfoLabel->setToolTip(image.isEmpty() ? QString() : nativePath(image));
    if (image != sfiImage) {
        runFirmwareInfo(image);
    }
    updateControls();
}

void FirmwareTab::Private::runFirmwareInfo(const QString& image)
{
    sfiImage = image;
    if (sfiInvocation) {
        disconnect(sfiInvocation, nullptr, q, nullptr);
        sfiInvocation->cancel();
        sfiInvocation = nullptr;
    }
    if (image.isEmpty()) {
        sfiText.clear();
    } else if (upgradeToolPath.isEmpty()) {
        sfiText = FirmwareTab::tr("upgrade_tool not found: no image details");
    } else {
        sfiText = FirmwareTab::tr("reading the image details...");
        ToolInvocation* invocation = flasher->upgradeTool()->firmwareInfo(image);
        sfiInvocation = invocation;
        connect(invocation, &ToolInvocation::finished, q, [this, invocation](const ToolResult& result) {
            if (sfiInvocation != invocation) {
                return;   // superseded by another image
            }
            sfiInvocation = nullptr;
            const RkFirmwareInfo info = RockchipTool::parseFirmwareInfo(result.outputText());
            if (info.isValid()) {
                sfiText = info.summary().isEmpty() ? info.type : info.summary();
            } else if (!result.started) {
                sfiText = FirmwareTab::tr("upgrade_tool could not be started: %1").arg(result.error);
            } else {
                sfiText = FirmwareTab::tr("no image details (upgrade_tool SFI exit code %1)").arg(result.exitCode);
            }
            renderSocImage();
        });
    }
    renderSocImage();
}

void FirmwareTab::Private::renderSocImage()
{
    socImageLabel->setText(sfiImage.isEmpty() ? FirmwareTab::tr("no image selected") : sfiText);
}

// =======================================================================================
// Rendering / enable states
// =======================================================================================

void FirmwareTab::Private::renderDeviceStatus()
{
    if (!hasDeviceStatus) {
        socStatusLabel->setText(FirmwareTab::tr("not checked yet"));
        installDriversButton->setVisible(false);
        return;
    }
    QString text = deviceStatus.text();
    if (text.isEmpty()) {
        switch (deviceStatus.mode) {
        case SocFlasher::DeviceStatus::Mode::None:
            text = FirmwareTab::tr("no board");
            break;
        case SocFlasher::DeviceStatus::Mode::NoDriver:
            text = deviceStatus.hint.isEmpty() ? FirmwareTab::tr("a Rockchip device is attached but no driver claims it")
                                               : deviceStatus.hint;
            break;
        case SocFlasher::DeviceStatus::Mode::Adb:
            text = FirmwareTab::tr("ADB device %1").arg(deviceStatus.serial);
            break;
        case SocFlasher::DeviceStatus::Mode::Loader:
            text = FirmwareTab::tr("Loader mode %1").arg(deviceStatus.detail).trimmed();
            break;
        case SocFlasher::DeviceStatus::Mode::Maskrom:
            text = FirmwareTab::tr("Maskrom mode %1").arg(deviceStatus.detail).trimmed();
            break;
        }
    }
    const bool noDriver = deviceStatus.mode == SocFlasher::DeviceStatus::Mode::NoDriver;
    QPalette palette = socStatusLabel->palette();
    palette.setColor(QPalette::WindowText, noDriver ? kErrorText : q->palette().color(QPalette::WindowText));
    socStatusLabel->setPalette(palette);
    socStatusLabel->setText(text);
    if (boardInfoText.isEmpty()) {
        socStatusLabel->setToolTip(deviceStatus.detail);
    }
    installDriversButton->setVisible(noDriver);
}

void FirmwareTab::Private::renderPackage()
{
    QPalette palette = packageInfoLabel->palette();
    if (mcuPackage.isValid()) {
        QString target = CcProtocol::targetName(mcuPackage.target);
        if (target.isEmpty() || target == QLatin1String("unknown")) {
            target = mcuPackage.targetText();
        }
        packageInfoLabel->setText(FirmwareTab::tr("%1 %2, %3 bytes, CRC OK")
                                      .arg(target, mcuPackage.versionText(), QLocale().toString(mcuPackage.size)));
        packageInfoLabel->setToolTip(mcuPackage.summary());
        palette.setColor(QPalette::WindowText, q->palette().color(QPalette::WindowText));
    } else if (!packageProblem.isEmpty()) {
        packageInfoLabel->setText(packageProblem);
        packageInfoLabel->setToolTip(packageProblem);
        palette.setColor(QPalette::WindowText, kErrorText);
    } else {
        packageInfoLabel->setText(FirmwareTab::tr("no package chosen"));
        packageInfoLabel->setToolTip(QString());
        palette.setColor(QPalette::WindowText, q->palette().color(QPalette::WindowText));
    }
    packageInfoLabel->setPalette(palette);
}

void FirmwareTab::Private::renderMcuStatus()
{
    if (runningVersion.isEmpty()) {
        mcuStatusLabel->setText(FirmwareTab::tr("unknown"));
    } else {
        mcuStatusLabel->setText(QStringLiteral("%1 %2").arg(runningVersion, runningRole).trimmed());
    }
}

bool FirmwareTab::Private::cancellable() const
{
    if (operation.isEmpty()) {
        return false;
    }
    if (step == kOpFlash) {
        return flashCancellable;   // never while upgrade_tool writes the flash
    }
    return step == kOpPull || step == kOpMcu || step == kOpSetRole || step == kOpInfo || cliInvocation;
}

void FirmwareTab::Private::updateControls()
{
    const bool idle = operation.isEmpty();
    for (QWidget* widget : {static_cast<QWidget*>(sourceCombo), static_cast<QWidget*>(profilesButton),
                            static_cast<QWidget*>(remoteFolderEdit), static_cast<QWidget*>(cacheDirEdit),
                            static_cast<QWidget*>(cacheDirButton), static_cast<QWidget*>(pullButton),
                            static_cast<QWidget*>(localImageButton), static_cast<QWidget*>(packageEdit),
                            static_cast<QWidget*>(packageButton), static_cast<QWidget*>(readMcuButton),
                            static_cast<QWidget*>(roleRxRadio), static_cast<QWidget*>(roleTxRadio),
                            static_cast<QWidget*>(pullAndFlashButton), static_cast<QWidget*>(installDriversButton)}) {
        widget->setEnabled(idle);
    }
    rollbackButton->setEnabled(idle && hasPreviousImage);
    flashSocButton->setEnabled(idle && !q->imagePath().isEmpty());
    // The role is what the user confirms: never guessed, so no update without one.
    updateMcuButton->setEnabled(idle && mcuPackage.isValid() && confirmedRole != 0);
    setRoleButton->setEnabled(idle && confirmedRole != 0);
    cancelButton->setEnabled(cancellable());
}

void FirmwareTab::Private::updateStatusTimer()
{
    if (refreshMs > 0 && q->isVisible() && operation.isEmpty()) {
        if (statusTimer.interval() != refreshMs) {
            statusTimer.setInterval(refreshMs);
        }
        if (!statusTimer.isActive()) {
            statusTimer.start();
        }
    } else {
        statusTimer.stop();
    }
}

void FirmwareTab::Private::pushToolPaths()
{
    flasher->upgradeTool()->setToolPath(upgradeToolPath);
    flasher->adb()->setAdbPath(adbPath);
    link->adb()->setAdbPath(adbPath);
}

void FirmwareTab::Private::logToolPaths()
{
    const AppSettings& s = AppSettings::instance();
    q->appendLog(FirmwareTab::tr("upgrade_tool: %1").arg(ToolLocator::describe(ToolLocator::Tool::UpgradeTool, s.firmwareUpgradeToolPath())));
    q->appendLog(FirmwareTab::tr("adb: %1").arg(ToolLocator::describe(ToolLocator::Tool::Adb, s.firmwareAdbPath())));
}

// =======================================================================================
// Operations: framework
// =======================================================================================

QString FirmwareTab::Private::operationTitle(const QString& op)
{
    if (op == kOpPull) {
        return FirmwareTab::tr("Pull");
    }
    if (op == kOpFlash) {
        return FirmwareTab::tr("Flash SoC");
    }
    if (op == kOpMcu) {
        return FirmwareTab::tr("Update MCU");
    }
    if (op == kOpSetRole) {
        return FirmwareTab::tr("Set role");
    }
    if (op == kOpInfo) {
        return FirmwareTab::tr("Read MCU");
    }
    if (op == kOpPullFlash) {
        return FirmwareTab::tr("Pull & Flash");
    }
    return op;
}

bool FirmwareTab::Private::refuseIfBusy()
{
    if (operation.isEmpty()) {
        return false;
    }
    emit q->statusMessage(FirmwareTab::tr("Busy: %1 is still running").arg(operationTitle(operation)), kStatusShortMs);
    return true;
}

void FirmwareTab::Private::beginOperation(const QString& op)
{
    operation = op;
    step.clear();
    cancelRequested = false;
    statusTimer.stop();   // no status queries while tools run
    q->appendLog(FirmwareTab::tr("--- %1 ---").arg(operationTitle(op)));
    setProgressBusy(operationTitle(op));
    setPersistent(QStringLiteral("%1...").arg(operationTitle(op)));
    updateControls();
    qCInfo(lcFirmware) << "operation" << op << "started";
    emit q->busyChanged(true);
}

void FirmwareTab::Private::stepFinished(const QString& stepName, bool ok, const QString& message)
{
    if (operation.isEmpty()) {
        return;   // a late report of an operation that already ended
    }
    step.clear();
    if (operation != kOpPullFlash) {
        endOperation(stepName, ok, message);
        return;
    }
    // Pull & Flash: report the step, then go on or stop at the first failure.
    q->appendLog(ok ? FirmwareTab::tr("%1: %2").arg(operationTitle(stepName), message)
                    : FirmwareTab::tr("%1 failed: %2").arg(operationTitle(stepName), message));
    emit q->operationFinished(stepName, ok, message);
    if (!ok) {
        keepSsh = false;
        closeSsh();
        endOperation(kOpPullFlash, false, FirmwareTab::tr("stopped at %1: %2").arg(operationTitle(stepName), message));
        return;
    }
    if (stepName == kOpPull) {
        startFlash();
    } else if (stepName == kOpFlash) {
        startChainMcu();
    } else {
        keepSsh = false;
        closeSsh();
        endOperation(kOpPullFlash, true, message);
    }
}

void FirmwareTab::Private::endOperation(const QString& stepName, bool ok, const QString& message)
{
    const QString op = operation;
    operation.clear();
    step.clear();
    persistent.clear();   // the result message below replaces the hint (MainWindow re-reads it)
    cancelRequested = false;
    flashCancellable = false;
    if (progressBar->maximum() == 0) {
        progressBar->setRange(0, 100);
        progressBar->setValue(ok ? 100 : 0);
    } else if (ok) {
        progressBar->setValue(100);
    }
    const QString text = ok ? FirmwareTab::tr("%1 done: %2").arg(operationTitle(op), message)
                            : FirmwareTab::tr("%1 failed: %2").arg(operationTitle(op), message);
    progressLabel->setText(text);
    q->appendLog(text);
    qCInfo(lcFirmware) << "operation" << op << (ok ? "succeeded" : "failed");
    updateControls();
    updateStatusTimer();
    emit q->busyChanged(false);
    emit q->statusMessage(text, kStatusLongMs);
    emit q->operationFinished(stepName, ok, message);
    if (ok && (stepName == kOpFlash || op == kOpPullFlash)) {
        q->refreshStatus();   // show the board as it came back
    }
}

void FirmwareTab::Private::setPersistent(const QString& text)
{
    if (text == persistent) {
        return;
    }
    persistent = text;
    emit q->statusMessage(text, 0);   // lasting while busy; MainWindow keeps it under transient messages
}

void FirmwareTab::Private::setProgressBusy(const QString& text)
{
    progressShowsReady = false;
    progressBar->setRange(0, 0);   // indeterminate
    progressLabel->setText(text);
}

void FirmwareTab::Private::setProgressPercent(int percent, const QString& text)
{
    progressShowsReady = false;
    if (progressBar->maximum() != 100) {
        progressBar->setRange(0, 100);
    }
    progressBar->setValue(qBound(0, percent, 100));
    progressLabel->setText(text);
}

// =======================================================================================
// Pull
// =======================================================================================

void FirmwareTab::Private::startPull()
{
    step = kOpPull;
    SshProfile profile;
    QString error;
    if (!resolveProfile(&profile, &error)) {
        stepFinished(kOpPull, false, error);
        return;
    }
    const QString folder = q->remoteFolder();
    if (folder.isEmpty()) {
        stepFinished(kOpPull, false, FirmwareTab::tr("enter the remote folder that holds update.img"));
        return;
    }
    const QString cache = q->cacheDirectory();
    if (cache.isEmpty() || !QDir().mkpath(cache)) {
        stepFinished(kOpPull, false, FirmwareTab::tr("cannot create the cache folder %1").arg(nativePath(cache)));
        return;
    }
    AppSettings& settings = AppSettings::instance();
    settings.setFirmwareSourceTarget(targetKey());
    settings.setFirmwareRemoteFolder(targetKey(), folder);
    settings.setFirmwareCacheDirectory(cache);

    // As SessionWidget::connectSsh(): the store first (its changed() rebuilds the combo, which keeps
    // the text), then the Preferences > SSH defaults fill what the profile leaves open.
    if (profiles) {
        if (!profile.id.isEmpty()) {
            profiles->touch(profile.id);
        } else {
            profiles->addRecentTarget(profile.displayTarget());
        }
    }
    if (profile.knownHostsFile.trimmed().isEmpty()) {
        profile.knownHostsFile = settings.sshKnownHostsFile();
    }
    if (profile.id.isEmpty()) {
        if (profile.identityFile.trimmed().isEmpty()) {
            profile.identityFile = settings.sshDefaultIdentityFile();
        }
        profile.terminalType = settings.sshDefaultTerminalType();
        profile.keepAliveSeconds = settings.sshDefaultKeepAliveSeconds();
    }
    profile.remoteCommand.clear();    // a plain session: the pull runs its own exec / SFTP channels
    profile.startupCommand.clear();
    profile.localForwards.clear();

    pullTarget = profile.displayTarget();
    if (ssh->state() == Transport::State::Connected && ssh->profile().displayTarget() == pullTarget) {
        startPuller();   // still open (a Pull & Flash, or a pull right after a cancelled one)
        return;
    }
    closeSsh();
    sshError.clear();
    ssh->setProfile(profile);
    pullPhase = PullPhase::Connecting;
    const QString text = FirmwareTab::tr("Pull: connecting to %1...").arg(pullTarget);
    setProgressBusy(text);
    setPersistent(text);
    updateControls();
    q->appendLog(FirmwareTab::tr("Connecting to %1").arg(pullTarget));
    qCInfo(lcFirmware) << "pull: connecting to" << pullTarget;
    if (!ssh->open()) {
        pullPhase = PullPhase::Idle;
        const QString reason = ssh->errorString();
        stepFinished(kOpPull, false, reason.isEmpty() ? FirmwareTab::tr("cannot connect to %1").arg(pullTarget) : reason);
    }
}

void FirmwareTab::Private::onSshStateChanged(Transport::State state)
{
    if (pullPhase == PullPhase::Connecting) {
        if (state == Transport::State::Connected) {
            q->appendLog(FirmwareTab::tr("Connected to %1 (%2)").arg(pullTarget, ssh->summary()));
            startPuller();
        } else if (state == Transport::State::Disconnected) {
            pullPhase = PullPhase::Idle;
            const QString reason = cancelRequested ? FirmwareTab::tr("cancelled")
                                   : sshError.isEmpty() ? FirmwareTab::tr("the connection to %1 closed").arg(pullTarget)
                                                        : sshError;
            stepFinished(kOpPull, false, reason);
        }
        return;
    }
    if (pullPhase == PullPhase::Pulling && state == Transport::State::Disconnected) {
        // The puller reports the dropped connection itself; make sure the step ends regardless.
        QTimer::singleShot(kSilentRefusalMs, q, [this]() {
            if (pullerActive && ssh->state() == Transport::State::Disconnected) {
                puller->cancel();
                pullFinished(false, sshError.isEmpty() ? FirmwareTab::tr("the connection to %1 closed").arg(pullTarget) : sshError);
            }
        });
    }
}

void FirmwareTab::Private::startPuller()
{
    pullPhase = PullPhase::Pulling;
    const AppSettings& settings = AppSettings::instance();
    puller->setConnection(ssh);
    puller->setRemoteFolder(q->remoteFolder());
    puller->setCacheDirectory(q->cacheDirectory());
    puller->setSdkDirectory(QString());   // derived from the remote folder
    puller->setGitDescribe(settings.firmwareGitDescribe());
    puller->setExtraFiles(settings.firmwareExtraFiles());
    puller->setForceDownload(false);
    const QString text = FirmwareTab::tr("Pull: checking update.img on %1...").arg(pullTarget);
    setProgressBusy(text);
    setPersistent(text);
    pullerActive = true;
    if (!puller->start()) {
        QTimer::singleShot(kSilentRefusalMs, q, [this]() {
            if (pullerActive && !puller->isRunning()) {
                pullFinished(false, FirmwareTab::tr("the image pull did not start"));
            }
        });
    }
}

void FirmwareTab::Private::pullFinished(bool ok, const QString& message)
{
    pullerActive = false;
    pullPhase = PullPhase::Idle;
    if (!keepSsh) {
        closeSsh();   // the connection is only needed while pulling
    }
    if (ok) {
        // The freshly pulled (or unchanged) cache image is what Flash SoC uses from now on.
        localImage.clear();
        AppSettings::instance().setFirmwareLocalImage(QString());
    }
    refreshImage(/*reread=*/ok);
    stepFinished(kOpPull, ok, message);
}

void FirmwareTab::Private::closeSsh()
{
    if (ssh->state() == Transport::State::Disconnected) {
        return;
    }
    if (pullPhase == PullPhase::Connecting) {
        pullPhase = PullPhase::Idle;   // the Disconnected that close() reports is no pull result
    }
    ssh->close();
    qCInfo(lcFirmware) << "build machine connection closed";
}

// =======================================================================================
// SoC
// =======================================================================================

void FirmwareTab::Private::startFlash()
{
    step = kOpFlash;
    const QString image = q->imagePath();
    if (image.isEmpty()) {
        stepFinished(kOpFlash, false, FirmwareTab::tr("no image: pull one or choose a local image first"));
        return;
    }
    pushToolPaths();
    flasher->setImagePath(image);
    boardInfoText.clear();
    q->appendLog(FirmwareTab::tr("Flashing %1").arg(nativePath(image)));
    const QString text = FirmwareTab::tr("Flashing SoC: starting...");
    setProgressBusy(text);
    setPersistent(text);
    flasherActive = true;
    flashCancellable = true;   // detecting the board is a waiting phase
    updateControls();
    if (!flasher->start()) {
        // A refusal reports its reason through finished() from the event loop (handled above);
        // one that stays silent (already running) ends the step here.
        QTimer::singleShot(kSilentRefusalMs, q, [this]() {
            if (flasherActive && !flasher->isRunning()) {
                flashFinished(false, FirmwareTab::tr("the SoC flasher did not start"));
            }
        });
    }
}

void FirmwareTab::Private::onFlasherPhase(SocFlasher::Phase phase)
{
    if (!flasherActive) {
        return;
    }
    QString text;
    bool waiting = false;
    switch (phase) {
    case SocFlasher::Phase::Detecting:
        text = FirmwareTab::tr("Flashing SoC: looking for the board");
        waiting = true;
        break;
    case SocFlasher::Phase::RebootingToLoader:
        text = FirmwareTab::tr("Flashing SoC: rebooting the board into Loader mode");
        waiting = true;
        break;
    case SocFlasher::Phase::WaitingForLoader:
        text = FirmwareTab::tr("Flashing SoC: waiting for the board in Loader / Maskrom mode");
        waiting = true;
        break;
    case SocFlasher::Phase::Flashing:
        text = FirmwareTab::tr("Flashing SoC: writing the flash - do not unplug the board");
        break;
    case SocFlasher::Phase::WaitingForAdb:
        text = FirmwareTab::tr("Flashing SoC: waiting for the board to come back on ADB");
        waiting = true;
        break;
    case SocFlasher::Phase::ReadingBoardInfo:
        text = FirmwareTab::tr("Flashing SoC: reading the board information");
        break;
    default:
        break;
    }
    flashCancellable = waiting;
    cancelButton->setEnabled(cancellable());
    if (!text.isEmpty()) {
        if (phase == SocFlasher::Phase::Flashing) {
            setProgressPercent(0, text);
        } else {
            setProgressBusy(text);
        }
        setPersistent(text);
    }
}

void FirmwareTab::Private::flashFinished(bool ok, const QString& message)
{
    flasherActive = false;
    flashCancellable = false;
    if (ok && !boardInfoText.isEmpty()) {
        socStatusLabel->setToolTip(boardInfoText);
    }
    stepFinished(kOpFlash, ok, message);
}

void FirmwareTab::Private::askDownloadMode()
{
    if (!flasherActive) {
        return;
    }
    QList<QPointer<SessionWidget>> connected;
    if (sessionProvider) {
        const QList<SessionWidget*> list = sessionProvider();
        for (SessionWidget* session : list) {
            if (session && session->isConnected()) {
                connected.append(session);
            }
        }
    }
    if (connected.isEmpty()) {
        showMaskromInstructions();
        return;
    }
    // One question naming the tab(s): Yes sends through the first, one more button per other tab.
    QMessageBox box(QMessageBox::Question, FirmwareTab::tr("Firmware Update"),
                    FirmwareTab::tr("No board is visible on USB (no ADB, Loader or Maskrom device).\n\n"
                       "Send \"reboot loader\" through the connected session %1 so the board restarts "
                       "in Loader mode?")
                        .arg(connected.first()->title()),
                    QMessageBox::Yes | QMessageBox::No, q->window());
    box.button(QMessageBox::Yes)->setText(FirmwareTab::tr("Send through %1").arg(connected.first()->title()));
    box.button(QMessageBox::No)->setText(FirmwareTab::tr("No, use the MASKROM button"));
    box.setDefaultButton(QMessageBox::Yes);
    QList<QAbstractButton*> extra;
    for (qsizetype i = 1; i < connected.size(); ++i) {
        extra.append(box.addButton(FirmwareTab::tr("Send through %1").arg(connected.at(i)->title()), QMessageBox::AcceptRole));
    }
    QPointer<FirmwareTab> self(q);
    const int answer = box.exec();
    if (!self || !flasherActive) {
        return;   // the tab went away or the flasher stopped waiting meanwhile
    }
    QPointer<SessionWidget> chosen;
    if (answer == QMessageBox::Yes) {
        chosen = connected.first();
    } else {
        const qsizetype index = extra.indexOf(box.clickedButton());
        if (index >= 0) {
            chosen = connected.at(index + 1);
        }
    }
    if (chosen && chosen->isConnected()) {
        chosen->sendBytes(QByteArrayLiteral("reboot loader\r"));
        q->appendLog(FirmwareTab::tr("Sent \"reboot loader\" through %1").arg(chosen->title()));
        return;
    }
    showMaskromInstructions();
}

void FirmwareTab::Private::showMaskromInstructions()
{
    const QString text = FirmwareTab::tr("No board found: hold the MASKROM button while plugging in the USB-C cable "
                            "(or while power-cycling the board); waiting up to %1 s")
                             .arg(flasher->waitTimeoutSeconds());
    q->appendLog(text);
    progressLabel->setText(text);
    setPersistent(text);
}

// =======================================================================================
// MCU
// =======================================================================================

void FirmwareTab::Private::startMcuUpdate(const QString& stepName)
{
    step = stepName;
    if (stepName == kOpMcu && !mcuPackage.isValid()) {
        stepFinished(stepName, false, FirmwareTab::tr("choose a valid .pkg package first"));
        return;
    }
    if (confirmedRole == 0) {
        stepFinished(stepName, false, FirmwareTab::tr("choose the MCU role (rx or tx) first: it is never guessed"));
        return;
    }
    mcuRole = confirmedRole;
    mcuResultPending = false;
    linkLostMessage.clear();
    if (stepName == kOpMcu) {
        updater->setPackage(mcuPackage);
        q->appendLog(FirmwareTab::tr("Updating the MCU with %1 (role %2)").arg(QFileInfo(mcuPackagePath).fileName(), roleText(mcuRole)));
    } else {
        q->appendLog(FirmwareTab::tr("Setting the MCU role to %1").arg(roleText(mcuRole)));
    }
    updater->setThroughSoc(true);
    pushToolPaths();
    const QString text = FirmwareTab::tr("%1: opening the link through the board...").arg(operationTitle(stepName));
    setProgressBusy(text);
    setPersistent(text);
    linkActive = true;
    updateControls();
    if (!link->open()) {
        QTimer::singleShot(kSilentRefusalMs, q, [this, stepName]() {
            if (linkActive && !updaterActive && !link->isOpening() && !link->isOpen() && step == stepName) {
                linkActive = false;
                stepFinished(stepName, false, FirmwareTab::tr("the MCU link could not be opened"));
            }
        });
    }
}

void FirmwareTab::Private::onLinkOpened()
{
    if (!linkActive || updaterActive || mcuResultPending) {
        return;
    }
    updater->setDevice(link->socket());
    updaterActive = true;
    const bool started = (step == kOpMcu) ? updater->flash(mcuRole) : updater->setRole(mcuRole);
    if (!started) {
        updaterActive = false;
        mcuDone(false, FirmwareTab::tr("the MCU updater did not start"));
    }
}

void FirmwareTab::Private::mcuDone(bool ok, const QString& message)
{
    mcuOk = ok;
    mcuMessage = message;
    mcuResultPending = true;   // before close(): closed() may come from inside it
    if (link->isOpen() || link->isOpening()) {
        linkCloseTimer.start(kLinkCloseTimeoutMs);
        link->close();
    } else {
        completeMcu();
    }
}

void FirmwareTab::Private::completeMcu()
{
    if (!mcuResultPending) {
        return;
    }
    linkCloseTimer.stop();
    mcuResultPending = false;
    linkActive = false;
    const QString stepName = step;
    if (mcuOk && stepName == kOpSetRole) {
        runningRole = roleText(mcuRole);
        renderMcuStatus();
    }
    stepFinished(stepName, mcuOk, mcuMessage);
}

void FirmwareTab::Private::startMcuRead(std::function<void(bool, const QString&)> done)
{
    cliDone = std::move(done);
    pendingVersion.clear();
    pushToolPaths();
    const QString text = FirmwareTab::tr("Reading the MCU version through ADB...");
    setProgressBusy(text);
    setPersistent(text);
    runCli(QStringLiteral("version"));
}

void FirmwareTab::Private::runCli(const QString& command)
{
    ToolInvocation* invocation = link->cliCommand(command);
    cliInvocation = invocation;
    q->appendLog(invocation->commandLine().isEmpty() ? FirmwareTab::tr("rfmcu cmd %1").arg(command) : invocation->commandLine());
    connect(invocation, &ToolInvocation::finished, q, [this, invocation, command](const ToolResult& result) {
        if (cliInvocation != invocation) {
            return;
        }
        cliInvocation = nullptr;
        onCliFinished(command, result);
    });
    updateControls();
}

void FirmwareTab::Private::onCliFinished(const QString& command, const ToolResult& result)
{
    const QString output = result.outputText().trimmed();
    if (!output.isEmpty()) {
        q->appendLog(output);
    }
    if (result.cancelled || cancelRequested) {
        finishCli(false, FirmwareTab::tr("cancelled"));
        return;
    }
    if (!result.started) {
        finishCli(false, FirmwareTab::tr("adb could not be started: %1").arg(result.error));
        return;
    }
    if (result.timedOut) {
        finishCli(false, FirmwareTab::tr("adb did not answer in time"));
        return;
    }
    // rfmcu cmd: 0 OK, 1 ERR, 2 port busy, 3 no reply (project/app/rfmcu/README.md).
    switch (result.exitCode) {
    case 0:
        break;
    case 2:
        finishCli(false, FirmwareTab::tr("the MCU port is busy (another rfmcu holds /dev/ttyS1): %1").arg(output));
        return;
    case 3:
        finishCli(false, FirmwareTab::tr("the MCU did not answer \"%1\"").arg(command));
        return;
    default:
        finishCli(false, FirmwareTab::tr("rfmcu cmd %1 failed (exit code %2): %3").arg(command).arg(result.exitCode).arg(output));
        return;
    }
    QString okLine;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.trimmed().startsWith(QLatin1String("OK"))) {
            okLine = line.trimmed();
            break;
        }
    }
    if (command == QLatin1String("version")) {
        const McuUpdater::CliFields fields = McuUpdater::parseCliFields(okLine);
        const QString version = fields.value(QStringLiteral("version"));
        if (version.isEmpty()) {
            finishCli(false, FirmwareTab::tr("unexpected reply to \"version\": %1").arg(output.isEmpty() ? FirmwareTab::tr("nothing") : output));
            return;
        }
        const QString role = fields.value(QStringLiteral("role"));
        if (role.isEmpty()) {
            pendingVersion = version;   // an older application: its "help" lists the role
            runCli(QStringLiteral("help"));
            return;
        }
        runningVersion = version;
        runningRole = role;
    } else {
        // fw_update.py probe_application(): the command words are the second word of every
        // "OK <name> <usage>" row of "help" (the real firmware prints one row per command, the
        // first one "OK commands:"); "tx" among them -> tx, "rx" -> rx, else unknown.
        // (Seen on the board 2026-10-13: 0.8.2 answers "OK version=0.8.2 baud=115200" without a
        // role, and its help lists "OK rx rx status | ...".)
        QStringList commands;
        for (const QString& line : lines) {
            const QStringList tokens = line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (tokens.size() > 1 && tokens.first() == QLatin1String("OK")) {
                commands << tokens.at(1);
            }
        }
        runningVersion = pendingVersion;
        runningRole = commands.contains(QLatin1String("tx"))   ? QStringLiteral("tx")
                      : commands.contains(QLatin1String("rx")) ? QStringLiteral("rx")
                                                               : QStringLiteral("unknown");
    }
    renderMcuStatus();
    const quint32 reported = roleFromText(runningRole);
    if (reported != 0) {
        q->setSelectedRole(reported);   // pre-selected: the radios show what the user confirms
    }
    finishCli(true, FirmwareTab::tr("MCU application version=%1 role=%2").arg(runningVersion, runningRole));
}

void FirmwareTab::Private::finishCli(bool ok, const QString& message)
{
    std::function<void(bool, const QString&)> done = std::move(cliDone);
    cliDone = nullptr;
    if (done) {
        done(ok, message);
    }
}

void FirmwareTab::Private::startChainMcu()
{
    if (!mcuPackage.isValid()) {
        q->appendLog(FirmwareTab::tr("No MCU package chosen: the MCU update is skipped"));
        keepSsh = false;
        closeSsh();
        endOperation(kOpPullFlash, true, FirmwareTab::tr("SoC flashed; no MCU package chosen"));
        return;
    }
    step = kOpMcu;
    startMcuRead([this](bool ok, const QString& message) {
        if (!ok) {
            stepFinished(kOpMcu, false, FirmwareTab::tr("cannot read the running MCU version: %1").arg(message));
            return;
        }
        if (runningVersion == mcuPackage.versionText()) {
            stepFinished(kOpMcu, true, FirmwareTab::tr("the MCU already runs %1; update skipped").arg(runningVersion));
            return;
        }
        startMcuUpdate(kOpMcu);
    });
}

// =======================================================================================
// Public slots
// =======================================================================================

void FirmwareTab::pull()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpPull);
    d->keepSsh = false;
    d->startPull();
}

void FirmwareTab::flashSoc()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpFlash);
    d->startFlash();
}

void FirmwareTab::updateMcu()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpMcu);
    d->startMcuUpdate(kOpMcu);
}

void FirmwareTab::setMcuRole()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpSetRole);
    d->startMcuUpdate(kOpSetRole);
}

void FirmwareTab::readMcuInfo()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpInfo);
    d->step = kOpInfo;
    d->startMcuRead([this](bool ok, const QString& message) { d->stepFinished(kOpInfo, ok, message); });
}

void FirmwareTab::pullAndFlash()
{
    if (d->refuseIfBusy()) {
        return;
    }
    d->beginOperation(kOpPullFlash);
    d->keepSsh = true;
    appendLog(tr("Pull & Flash: pull the image, flash the SoC, wait for ADB, then update the MCU if needed"));
    d->startPull();
}

void FirmwareTab::cancel()
{
    if (!isBusy()) {
        return;
    }
    if (d->step == kOpFlash && !d->flashCancellable) {
        const QString message = tr("Cannot cancel while upgrade_tool writes the flash");
        appendLog(message);
        emit statusMessage(message, kStatusLongMs);
        return;
    }
    d->cancelRequested = true;
    appendLog(tr("Cancelling %1").arg(Private::operationTitle(d->step.isEmpty() ? d->operation : d->step)));
    if (d->step == kOpPull) {
        if (d->pullPhase == Private::PullPhase::Connecting) {
            d->ssh->close();   // reported as Disconnected -> the step ends "cancelled"
        } else if (d->pullerActive) {
            d->puller->cancel();   // reports finished(false, "cancelled") once the transfer is undone
            QTimer::singleShot(kPullCancelGraceMs, this, [this]() {
                if (d->pullerActive) {
                    d->pullFinished(false, tr("cancelled"));
                }
            });
        }
        return;
    }
    if (d->step == kOpFlash) {
        d->flasher->cancel();
        QTimer::singleShot(kSilentRefusalMs, this, [this]() {
            if (d->flasherActive && !d->flasher->isRunning()) {
                d->flashFinished(false, tr("cancelled"));
            }
        });
        return;
    }
    if (d->cliInvocation) {
        d->cliInvocation->cancel();   // its finished() reports "cancelled"
        return;
    }
    if (d->step == kOpMcu || d->step == kOpSetRole) {
        if (d->updaterActive) {
            d->updater->cancel();   // finished(false, "cancelled") closes the link
        } else if (d->linkActive && !d->mcuResultPending) {
            d->mcuDone(false, tr("cancelled"));
        }
    }
}

void FirmwareTab::rollBack()
{
    if (d->refuseIfBusy()) {
        return;
    }
    QString error;
    if (!ImagePuller::rollBack(cacheDirectory(), &error)) {
        const QString message = tr("Roll back failed: %1").arg(error.isEmpty() ? tr("there is no previous image") : error);
        appendLog(message);
        emit statusMessage(message, kStatusLongMs);
        d->refreshImage();
        return;
    }
    d->localImage.clear();
    AppSettings::instance().setFirmwareLocalImage(QString());
    const QString message = tr("Rolled back: the previous image is the current one again");
    appendLog(message);
    emit statusMessage(message, kStatusShortMs);
    d->refreshImage(/*reread=*/true);
}

void FirmwareTab::chooseLocalImage()
{
    const QString current = imagePath();
    const QString start = current.isEmpty() ? cacheDirectory() : QFileInfo(current).absolutePath();
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose a Rockchip Update Image"), start,
                                                      tr("Rockchip update images (*.img);;All files (*)"));
    if (!file.isEmpty()) {
        setImagePath(file);
    }
}

void FirmwareTab::choosePackage()
{
    const QString start = d->mcuPackagePath.isEmpty() ? QString() : QFileInfo(d->mcuPackagePath).absolutePath();
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose a CC1310 Firmware Package"), start,
                                                      tr("CC1310 firmware packages (*.pkg);;All files (*)"));
    if (!file.isEmpty()) {
        setPackagePath(file);
    }
}

void FirmwareTab::refreshStatus()
{
    if (isBusy()) {
        return;   // upgrade_tool / adb belong to the running operation
    }
    d->flasher->queryStatus();
}

void FirmwareTab::installDrivers()
{
    QString installer = d->driverInstallerPath;
    if (installer.isEmpty() && !d->toolPathsPinned) {
        installer = ToolLocator::locate(ToolLocator::Tool::DriverInstaller);
    }
    if (installer.isEmpty() || !QFileInfo(installer).isFile()) {
        const QString expected = installer.isEmpty() ? ToolLocator::bundledPath(ToolLocator::Tool::DriverInstaller) : installer;
        const QString message = tr("The Rockchip driver installer is missing (%1); install DriverAssistant by hand")
                                    .arg(nativePath(expected));
        appendLog(message);
        emit statusMessage(message, kStatusLongMs);
        return;
    }
    appendLog(tr("Starting the Rockchip driver installer %1").arg(nativePath(installer)));
    ToolRunner::Options options;
    options.workingDirectory = QFileInfo(installer).absolutePath();
    ToolInvocation* invocation = d->runner->run(installer, {}, options);
    connect(invocation, &ToolInvocation::finished, this, [this, installer](const ToolResult& result) {
        if (!result.started) {
            const QString message = tr("Could not start %1: %2. Run it as administrator from that folder.")
                                        .arg(nativePath(installer), result.error);
            appendLog(message);
            emit statusMessage(message, kStatusLongMs);
            return;
        }
        appendLog(tr("The driver installer ended (exit code %1); replug the board").arg(result.exitCode));
        refreshStatus();
    });
}

void FirmwareTab::appendLog(const QString& line)
{
    QString text = line;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    for (const QString& part : lines) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty() && lines.size() > 1) {
            continue;   // blank lines inside multi-line tool output
        }
        const QString stamped = stamp + QLatin1Char(' ') + trimmed;
        d->logView->appendPlainText(stamped);
        qCInfo(lcFirmware).noquote() << trimmed;
        emit logLineAdded(stamped);
    }
    d->lastLogWasProgress = false;
}

void FirmwareTab::Private::appendToolLine(const QString& line)
{
    // upgrade_tool repeats "Download Image... (42%)" hundreds of times: one log line per stage,
    // updated in place, keeps the pane readable (the System Log gets the stage once).
    static const QRegularExpression progressLine(QStringLiteral(R"(^(.*?)\s*\((\d{1,3})%\)\s*$)"));
    const QString trimmed = line.trimmed();
    const QRegularExpressionMatch match = progressLine.match(trimmed);
    if (!match.hasMatch()) {
        q->appendLog(line);
        return;
    }
    const QString stage = match.captured(1);
    if (lastLogWasProgress && stage == lastProgressStage && logView->document()->blockCount() > 0) {
        const QString stamped = QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QLatin1Char(' ') + trimmed;
        QTextCursor cursor(logView->document()->lastBlock());
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursor.insertText(stamped);
        qCDebug(lcFirmware).noquote() << trimmed;
        emit q->logLineAdded(stamped);
        return;
    }
    q->appendLog(line);
    lastLogWasProgress = true;
    lastProgressStage = stage;
}

void FirmwareTab::clearLog()
{
    d->logView->clear();
    d->lastLogWasProgress = false;
}

void FirmwareTab::saveLog()
{
    const QString dir = AppSettings::instance().logDirectory();
    QDir().mkpath(dir);
    const QString suggested = QDir(dir).filePath(
        QStringLiteral("firmware_%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Firmware Log"), suggested,
                                                      tr("Text files (*.txt *.log);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(d->logView->toPlainText().toUtf8()) < 0 ||
        !file.commit()) {
        const QString message = tr("Cannot write %1: %2").arg(nativePath(path), file.errorString());
        qCWarning(lcFirmware) << "cannot save the firmware log to" << path << file.errorString();
        emit statusMessage(message, kStatusLongMs);
        return;
    }
    emit statusMessage(tr("Log saved to %1").arg(nativePath(path)), kStatusShortMs);
}

void FirmwareTab::applyPreferences()
{
    const AppSettings& s = AppSettings::instance();
    if (!d->toolPathsPinned) {
        const QString upgradeTool = ToolLocator::locate(ToolLocator::Tool::UpgradeTool, s.firmwareUpgradeToolPath());
        const QString adb = ToolLocator::locate(ToolLocator::Tool::Adb, s.firmwareAdbPath());
        const bool changed = upgradeTool != d->upgradeToolPath || adb != d->adbPath || d->logView->document()->isEmpty();
        d->upgradeToolPath = upgradeTool;
        d->adbPath = adb;
        d->driverInstallerPath = ToolLocator::locate(ToolLocator::Tool::DriverInstaller);
        d->pushToolPaths();
        if (changed) {
            d->logToolPaths();
            d->runFirmwareInfo(imagePath());
        }
    }
    d->flasher->setWaitTimeoutSeconds(s.firmwareLoaderWaitSeconds());
    d->flasher->setAdbReturnTimeoutSeconds(s.firmwareAdbReturnSeconds());
    const QString cache = s.firmwareCacheDirectory();
    if (cache != cacheDirectory()) {
        d->cacheDirEdit->setText(nativePath(cache));
        d->refreshImage();
    }
}

void FirmwareTab::disconnectSource()
{
    if (d->step == kOpPull && isBusy()) {
        cancel();
    }
    d->keepSsh = false;
    d->closeSsh();
}

// =======================================================================================
// Events
// =======================================================================================

void FirmwareTab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    d->updateStatusTimer();
    if (d->refreshMs > 0) {
        // A first look soon after the tab appears, not inside the show: whoever just made the tab
        // visible may still switch the refresh off (the tests do) or start an operation.
        QTimer::singleShot(kFirstRefreshDelayMs, this, [this]() {
            if (d->refreshMs > 0 && isVisible()) {
                refreshStatus();
            }
        });
    }
}

void FirmwareTab::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    d->statusTimer.stop();
}

void FirmwareTab::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        d->retranslate();
        emit titleChanged(title());
    }
    QWidget::changeEvent(event);
}
