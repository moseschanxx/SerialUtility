#include "ui/FirmwareTab.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
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
#include <QTableWidget>
#include <QTableWidgetItem>
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
#include "firmware/BoardProfile.h"
#include "firmware/CcProtocol.h"
#include "firmware/FirmwareJob.h"
#include "firmware/InventoryRecorder.h"
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
constexpr int kTargetComboMinWidth = 220;

// The restore-key prefixes of SessionWidget::restoreKey(), accepted by setSourceTarget().
const QLatin1String kProfileKeyPrefix("ssh:profile:");
const QLatin1String kTargetKeyPrefix("ssh:target:");

/// AppSettings.h has no accessor for the confirmed MCU role, so it lives next to the other
/// firmware keys in plain QSettings ("rx" / "tx"; empty = none confirmed). The next session shows
/// it in the radios without confirming it (loadSettings()).
const QLatin1String kRoleSettingsKey("firmware/role");

const QColor kErrorText(0xC0, 0x39, 0x2B);

/// The columns of partitionTable(); column 0 is the checkbox (its item carries the name as
/// Qt::UserRole data).
enum PartitionColumn { ColCheck, ColName, ColFile, ColOffset, ColSize, ColImageSize, PartitionColumnCount };
/// The table shows up to this many rows before it scrolls, so it never pushes the log pane away.
constexpr int kPartitionTableVisibleRows = 8;

/// A partition size as mtdparts writes it: "64 KiB", "3 MiB", "to the end".
QString layoutSizeText(quint64 bytes)
{
    constexpr quint64 kKiB = 1024;
    constexpr quint64 kMiB = 1024 * 1024;
    if (bytes == 0) {
        return QCoreApplication::translate("FirmwareTab", "to the end");
    }
    if (bytes % kMiB == 0) {
        return QStringLiteral("%1 MiB").arg(bytes / kMiB);
    }
    if (bytes % kKiB == 0) {
        return QStringLiteral("%1 KiB").arg(bytes / kKiB);
    }
    return QStringLiteral("%1 B").arg(QLocale().toString(static_cast<qulonglong>(bytes)));
}

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

/// Decimal units in the C locale, exactly as ImagePuller words its log lines ("11.4 MB"), so the
/// labels never show one image in two units.
QString formatSize(qint64 bytes)
{
    return QLocale::c().formattedDataSize(qMax<qint64>(0, bytes), 1, QLocale::DataSizeSIFormat);
}

QString formatTime(const QDateTime& time)
{
    return time.isValid() ? time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
}

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
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

    FirmwareTab* q;

    // ---- Collaborators ---------------------------------------------------------------------
    SshProfileStore* profiles = nullptr;
    FirmwareJob* job = nullptr;     ///< the chain: every operation, its collaborators and their state
    SessionProvider sessionProvider;
    QString driverInstallerPath;
    bool toolPathsPinned = false;   ///< setToolPaths() was called: applyPreferences() leaves the paths alone
    QTimer statusTimer;
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
    QLabel* profileCaption = nullptr;       ///< (v0.8.0) "Board profile:"
    QComboBox* profileCombo = nullptr;
    QLabel* profileLabel = nullptr;
    QCheckBox* verifyCheckBox = nullptr;    ///< (v0.8.0, verify-rl) "Verify after writing", remembered
    QLabel* socImageCaption = nullptr;
    QLabel* socImageLabel = nullptr;
    QLabel* partitionsCaption = nullptr;
    QLabel* partitionsLabel = nullptr;
    QPushButton* partitionsAllButton = nullptr;
    QPushButton* partitionsNoneButton = nullptr;
    QTableWidget* partitionTable = nullptr;
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

    // ---- View state ------------------------------------------------------------------------------
    QString lastTargetKey;          ///< targetKey() the remote folder was last loaded for
    QString sfiImage;               ///< the image the SoC image line describes
    QString sfiText;                ///< its SFI summary (or why there is none)
    QPointer<ToolInvocation> sfiInvocation;
    bool hasPreviousImage = false;
    QString packageProblem;         ///< why the last package was refused (shown while none is loaded)
    quint32 rememberedRole = 0;     ///< last session's role, shown in the radios but not confirmed
    bool restoring = false;         ///< loading the remembered values: nothing is written back
    bool progressShowsReady = true; ///< the progress label still says "Ready" (retranslated with the rest)
    bool settingRole = false;       ///< setSelectedRole() is handing the role to the job
    QPointer<QMessageBox> downloadBox;   ///< the open "send reboot loader through <tab>" question
    QPointer<QMessageBox> profileBox;    ///< (v0.8.0) the open question of a board-profile rule
    QString dailyImageKey;               ///< (v0.8.0) image, mtime and (fix4) profile id the daily checks were last decided for
    bool lastLogWasProgress = false;
    QString lastProgressStage;

    // ---- Construction ------------------------------------------------------------------------
    void setupUi();
    void wireWidgets();
    void wireJob();
    void retranslate();
    void loadSettings();

    // ---- Source -----------------------------------------------------------------------------
    void rebuildTargets();
    QString selectedProfileId() const;
    QString targetKey() const;
    void onTargetChanged(bool persist);
    void syncJob();                 ///< the inputs an operation starts with: the fields and Preferences
    void renderImage(bool reread);  ///< the image line (imageChanged()); reread: describe the same path again
    void runFirmwareInfo(const QString& image);
    void renderSocImage();

    // ---- Status / rendering ------------------------------------------------------------------
    void renderPartitions();
    void onPartitionItemChanged(QTableWidgetItem* item);
    void renderDeviceStatus();
    void renderPackage();
    void renderMcuStatus();
    void showRole(quint32 role);    ///< the radios only; the job's role is the caller's business
    void updateControls();
    void updateStatusTimer();
    void logToolPaths();

    // ---- The job's reports and questions -----------------------------------------------------
    void onProgress(const QString& stage, int percent);
    void onStepFinished(const QString& step, bool ok, const QString& message);
    void onOperationFinished(const QString& op, bool ok, const QString& message);
    FirmwareJob::HostKeyDecision askHostKey(const SshConnection::HostKeyInfo& info);
    std::optional<FirmwareJob::AuthAnswer> askAuthPrompt(const SshConnection::AuthPrompt& prompt);
    FirmwareJob::DownloadModeAnswer askDownloadMode();

    // ---- Board profiles (v0.8.0) ------------------------------------------------------------
    QString selectedProfileOverride() const;
    void renderProfile();
    void renderImageDetails();
    void applyDailyPartitions();
    bool askProfileQuestion(const QString& refusal, const QString& flag, const QString& question, const QString& accept);

    void appendLines(const QString& line, bool mirror);
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
    d->job = new FirmwareJob(runner, this);   // `runner` null: the job owns a ProcessToolRunner
    d->job->setProfileStore(profiles);
    // (v0.8.0) The bench inventory: every status and operation of this tab records its board
    // (parented to the tab; its lines go to the log pane).
    auto* recorder = new InventoryRecorder(d->job, this);
    connect(recorder, &InventoryRecorder::logLine, this, &FirmwareTab::appendLog);

    d->statusTimer.setInterval(d->refreshMs);
    connect(&d->statusTimer, &QTimer::timeout, this, &FirmwareTab::refreshStatus);

    d->setupUi();
    d->wireWidgets();
    d->wireJob();
    if (d->profiles) {
        connect(d->profiles, &SshProfileStore::changed, this, [this]() { d->rebuildTargets(); });
        connect(d->profiles, &QObject::destroyed, this, [this]() {
            d->profiles = nullptr;
            d->job->setProfileStore(nullptr);
            d->rebuildTargets();
        });
    }
    d->rebuildTargets();
    d->retranslate();
    d->loadSettings();
    applyPreferences();
    if (d->rememberedRole != 0) {
        appendLog(tr("Role %1 remembered from the last session: click it or press Read to confirm it")
                      .arg(FirmwareJob::roleText(d->rememberedRole)));
    }
    d->updateControls();
}

FirmwareTab::~FirmwareTab()
{
    // Children are destroyed after `d`: the line edits emit editingFinished when they lose the
    // focus on the way out and the job may still report, so every connection into this object is
    // cut before anything is stopped.
    d->statusTimer.stop();
    const QList<QObject*> children = findChildren<QObject*>();
    for (QObject* child : children) {
        disconnect(child, nullptr, this, nullptr);
    }
    if (d->profiles) {
        disconnect(d->profiles, nullptr, this, nullptr);
    }
    if (d->sfiInvocation) {
        disconnect(d->sfiInvocation, nullptr, this, nullptr);
        d->sfiInvocation->cancel();
    }
    // The job stops what still runs (a pull, an MCU update, a flash that is only waiting), closes
    // the MCU link - its board cleanup goes out detached - and the build-machine connection; now,
    // while the tab is whole, as before the split. MainWindow never closes a tab that writes.
    delete d->job;
    d->job = nullptr;
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
    // (v0.8.0) The board profile next to the status line, whose text stays the 0.7.0 one.
    profileCaption = new QLabel(q);
    profileCombo = new QComboBox(q);
    profileCombo->setObjectName(QStringLiteral("profileCombo"));
    profileCombo->addItem(QString(), QString());   // "Auto-detect" (retranslate())
    for (const BoardProfile& profile : BoardProfiles::all()) {
        profileCombo->addItem(profile.name, profile.id);
    }
    profileCaption->setBuddy(profileCombo);
    profileLabel = makeValueLabel("profileLabel");
    auto* deviceRow = makeRow();
    deviceRow->addWidget(deviceCaption);
    deviceRow->addWidget(socStatusLabel, 1);
    deviceRow->addWidget(installDriversButton);
    deviceRow->addSpacing(8);
    deviceRow->addWidget(profileCaption);
    deviceRow->addWidget(profileCombo);
    deviceRow->addWidget(profileLabel, 1);
    socLayout->addLayout(deviceRow);

    socImageCaption = new QLabel(q);
    socImageLabel = makeValueLabel("socImageLabel");
    auto* socImageRow = makeRow();
    socImageRow->addWidget(socImageCaption);
    socImageRow->addWidget(socImageLabel, 1);
    socLayout->addLayout(socImageRow);

    // The partitions of the image, SocToolKit-style: one checkbox per row.
    partitionsCaption = new QLabel(q);
    partitionsLabel = makeValueLabel("partitionsLabel");
    partitionsAllButton = makeButton("partitionsAllButton");
    partitionsNoneButton = makeButton("partitionsNoneButton");
    auto* partitionsRow = makeRow();
    partitionsRow->addWidget(partitionsCaption);
    partitionsRow->addWidget(partitionsLabel, 1);
    partitionsRow->addWidget(partitionsAllButton);
    partitionsRow->addWidget(partitionsNoneButton);
    socLayout->addLayout(partitionsRow);
    partitionTable = new QTableWidget(0, PartitionColumnCount, q);
    partitionTable->setObjectName(QStringLiteral("partitionTable"));
    partitionTable->setSelectionMode(QAbstractItemView::NoSelection);
    partitionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    partitionTable->setAlternatingRowColors(true);
    partitionTable->setShowGrid(false);
    partitionTable->setWordWrap(false);
    partitionTable->verticalHeader()->setVisible(false);
    QHeaderView* header = partitionTable->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setStretchLastSection(true);
    header->setHighlightSections(false);
    const int rowHeight = partitionTable->verticalHeader()->defaultSectionSize();
    partitionTable->setMaximumHeight(header->sizeHint().height() + kPartitionTableVisibleRows * rowHeight
                                     + 2 * partitionTable->frameWidth());
    partitionTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    socLayout->addWidget(partitionTable);

    flashSocButton = makeButton("flashSocButton");
    cancelButton = makeButton("cancelButton");
    verifyCheckBox = new QCheckBox(q);
    verifyCheckBox->setObjectName(QStringLiteral("verifyCheckBox"));   // (v0.8.0) next to Flash SoC
    auto* flashRow = makeRow();
    flashRow->addWidget(flashSocButton);
    flashRow->addWidget(cancelButton);
    flashRow->addWidget(verifyCheckBox);
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
        job->setRemoteFolder(folder);
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
    connect(partitionsAllButton, &QPushButton::clicked, q, [this]() { job->selectAllPartitions(); });
    // "None" has no SocFlasher form: the job keeps it and Flash SoC refuses it.
    connect(partitionsNoneButton, &QPushButton::clicked, q, [this]() { job->setSelectedPartitions({}); });
    connect(partitionTable, &QTableWidget::itemChanged, q, [this](QTableWidgetItem* item) { onPartitionItemChanged(item); });
    // (v0.8.0) The board profile box: remembered, and the job's override at once.
    connect(profileCombo, &QComboBox::currentIndexChanged, q, [this](int) {
        const QString id = selectedProfileOverride();
        if (!restoring && id != AppSettings::instance().firmwareProfile()) {
            AppSettings::instance().setFirmwareProfile(id);
        }
        job->setProfileOverride(id);
    });
    // (v0.8.0) Verify after writing: remembered, and the job's choice at once (syncJob() hands it
    // over again before every operation).
    connect(verifyCheckBox, &QCheckBox::toggled, q, [this](bool on) {
        if (!restoring && on != AppSettings::instance().firmwareVerifyAfterWrite()) {
            AppSettings::instance().setFirmwareVerifyAfterWrite(on);
        }
        job->setVerifyAfterWrite(on);
    });

    // ---- MCU ----
    connect(packageEdit, &QLineEdit::editingFinished, q, [this]() {
        const QString typed = QDir::fromNativeSeparators(packageEdit->text().trimmed());
        if (typed != job->packagePath()) {
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

void FirmwareTab::Private::wireJob()
{
    // ---- The log, the progress row, the status bar ----
    connect(job, &FirmwareJob::logLine, q, [this](const QString& line) { appendLines(line, /*mirror=*/false); });   // the job mirrors its own
    connect(job, &FirmwareJob::toolOutput, q, [this](const QString& line) { appendToolLine(line); });
    connect(job, &FirmwareJob::progress, q, [this](const QString& stage, int percent) { onProgress(stage, percent); });
    connect(job, &FirmwareJob::persistentStatusChanged, q, [this](const QString& text) {
        emit q->statusMessage(text, 0);   // lasting while busy; MainWindow keeps it under transient messages
    });
    connect(job, &FirmwareJob::busyRefused, q, [this](const QString& message) { emit q->statusMessage(message, kStatusShortMs); });
    connect(job, &FirmwareJob::cancelRefused, q, [this](const QString& message) { emit q->statusMessage(message, kStatusLongMs); });
    connect(job, &FirmwareJob::busyChanged, q, [this](bool busy) {
        if (busy) {
            statusTimer.stop();   // no status queries while tools run
        } else if (profileBox) {
            profileBox->reject();   // the operation ended meanwhile: the question is moot
        }
        updateControls();
        if (!busy) {
            updateStatusTimer();
        }
        emit q->busyChanged(busy);
    });
    connect(job, &FirmwareJob::cancellableChanged, q, [this](bool cancellable) { cancelButton->setEnabled(cancellable); });
    connect(job, &FirmwareJob::stepFinished, q,
            [this](const QString& step, bool ok, const QString& message) { onStepFinished(step, ok, message); });
    connect(job, &FirmwareJob::operationFinished, q,
            [this](const QString& op, bool ok, const QString& message) { onOperationFinished(op, ok, message); });

    // ---- What the sections show ----
    connect(job, &FirmwareJob::statusChanged, q, [this](const SocFlasher::DeviceStatus&) { renderDeviceStatus(); });
    connect(job, &FirmwareJob::boardInfoReceived, q, [this](const QString& text) { socStatusLabel->setToolTip(text); });
    connect(job, &FirmwareJob::imageChanged, q, [this](bool reread) { renderImage(reread); });
    connect(job, &FirmwareJob::packageChanged, q, [this]() {
        renderPackage();
        updateControls();
    });
    connect(job, &FirmwareJob::roleChanged, q, [this](quint32 role) {
        if (!settingRole) {
            q->setSelectedRole(role);   // Read pre-selected the reported role: the radios show what the user confirms
        }
    });
    connect(job, &FirmwareJob::mcuInfoChanged, q, [this]() { renderMcuStatus(); });
    connect(job, &FirmwareJob::partitionsChanged, q, [this]() { renderPartitions(); });
    connect(job, &FirmwareJob::partitionsPlanned, q, [this]() {
        applyDailyPartitions();
        renderImageDetails();
    });
    connect(job, &FirmwareJob::boardProfileChanged, q, [this](const ProfileDetection&) {
        renderProfile();
        applyDailyPartitions();   // (fix4, m3) the profile usually arrives after the image's plan
    });
    connect(job->socFlasher(), &SocFlasher::phaseChanged, q, [this](SocFlasher::Phase phase) {
        if (downloadBox && phase != SocFlasher::Phase::WaitingForLoader) {
            downloadBox->reject();   // the question is moot: the job sends nothing
        }
    });

    // ---- What the settings remember ----
    connect(job, &FirmwareJob::pullSourceAccepted, q, [](const QString& target, const QString& folder, const QString& cache) {
        AppSettings& settings = AppSettings::instance();
        settings.setFirmwareSourceTarget(target);
        settings.setFirmwareRemoteFolder(target, folder);
        settings.setFirmwareCacheDirectory(cache);
    });
    connect(job, &FirmwareJob::mtdPartsRead, q, [](const QString& serial, const QString& mtdparts) {
        AppSettings& settings = AppSettings::instance();
        if (!serial.isEmpty() && settings.firmwareMtdParts(serial) != mtdparts) {
            settings.setFirmwareMtdParts(serial, mtdparts);
        }
        if (settings.firmwareMtdParts(QString()) != mtdparts) {
            settings.setFirmwareMtdParts(QString(), mtdparts);   // "*": the layout for a board met in Loader mode
        }
    });
    job->setLayoutProvider([](const QString& serial) { return AppSettings::instance().firmwareMtdParts(serial); });

    // ---- The questions: the same dialogs as a session tab ----
    job->setHostKeyHandler([this](const SshConnection::HostKeyInfo& info) { return askHostKey(info); });
    job->setAuthPromptHandler([this](const SshConnection::AuthPrompt& prompt) { return askAuthPrompt(prompt); });
    job->setDownloadModeHandler([this]() { return askDownloadMode(); });
    // (v0.8.0) The board profiles' three questions (the rules' overrides of su-fw's flags).
    job->setLayoutConflictHandler([this](const QString& imageLayout, const QString& boardLayout) {
        return askProfileQuestion(ProfileRules::layoutRefusal(imageLayout, boardLayout, false), QStringLiteral("--force-layout"),
                                  FirmwareTab::tr("Write anyway?"), FirmwareTab::tr("Write the image's layout"));
    });
    job->setLoaderPartitionsHandler([this](const BoardProfile& profile, const QStringList& names) {
        return askProfileQuestion(ProfileRules::loaderPartitionRefusal(profile, names, false),
                                  QStringLiteral("--force-loader-partitions"), FirmwareTab::tr("Write them anyway?"),
                                  FirmwareTab::tr("Write them"));
    });
    job->setSocPowerHandler([this](const BoardProfile& profile) {
        return askProfileQuestion(ProfileRules::socPowerRefusal(profile, false), QStringLiteral("--confirm-soc-power"),
                                  FirmwareTab::tr("Confirm only once the carrier's CC1310 bootloader is known to hold "
                                                  "SOC_PWR_EN (the 14 Oct 2026 build or later). Proceed?"),
                                  FirmwareTab::tr("Proceed"));
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
    profileCaption->setText(FirmwareTab::tr("Board profile:"));
    profileCombo->setItemText(0, FirmwareTab::tr("Auto-detect"));
    profileCombo->setToolTip(FirmwareTab::tr("The attached board when the tool cannot tell it; Auto-detect reads it from the "
                                             "board's Loader PID, product string or flash layout"));
    verifyCheckBox->setText(FirmwareTab::tr("Verify after writing"));
    verifyCheckBox->setToolTip(FirmwareTab::tr("Read every partition back after it was written (upgrade_tool RL) and compare it with "
                                               "the image byte for byte; a difference fails the flash and leaves the board in Loader "
                                               "mode with nothing else written. Costs about one read per write; MASKROM mode (UF) "
                                               "checks the image itself"));
    installDriversButton->setText(FirmwareTab::tr("Install drivers"));
    installDriversButton->setToolTip(FirmwareTab::tr("Run the bundled Rockchip DriverAssistant (rockusb and ADB drivers)"));
    socImageCaption->setText(FirmwareTab::tr("Image:"));
    partitionsCaption->setText(FirmwareTab::tr("Partitions:"));
    partitionsAllButton->setText(FirmwareTab::tr("All"));
    partitionsAllButton->setToolTip(FirmwareTab::tr("Check every partition that can be written"));
    partitionsNoneButton->setText(FirmwareTab::tr("None"));
    partitionsNoneButton->setToolTip(FirmwareTab::tr("Uncheck every partition"));
    partitionTable->setHorizontalHeaderLabels({QString(), FirmwareTab::tr("Partition"), FirmwareTab::tr("Image file"),
                                               FirmwareTab::tr("Offset"), FirmwareTab::tr("Size"),
                                               FirmwareTab::tr("Image size")});
    partitionTable->setToolTip(FirmwareTab::tr("The partitions of the image: in Loader mode Flash SoC writes the checked ones "
                                               "(MASKROM mode writes the whole image)"));
    flashSocButton->setText(FirmwareTab::tr("Flash SoC"));
    flashSocButton->setToolTip(FirmwareTab::tr("Write the image to the Rockchip SoC over the USB-C cable: the checked partitions "
                                               "in Loader mode, the whole image in MASKROM mode"));
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

    if (!job->isBusy() && progressShowsReady) {
        progressLabel->setText(FirmwareTab::tr("Ready"));
    }
    job->refreshImage();
    renderSocImage();
    renderPartitions();
    renderDeviceStatus();
    renderProfile();
    renderImageDetails();
    renderPackage();
    renderMcuStatus();
}

void FirmwareTab::Private::loadSettings()
{
    const AppSettings& s = AppSettings::instance();
    restoring = true;
    cacheDirEdit->setText(nativePath(s.firmwareCacheDirectory()));
    job->setCacheDirectory(s.firmwareCacheDirectory());
    q->setSourceTarget(s.firmwareSourceTarget());   // also loads that target's remote folder

    const QString local = s.firmwareLocalImage();
    if (!local.isEmpty() && QFileInfo(local).isFile() && RockchipTool::isRkfwImage(local)) {
        job->useLocalImage(local);
    }
    const QString packageFile = s.firmwarePackagePath();
    if (!packageFile.isEmpty() && QFileInfo(packageFile).isFile()) {
        CcPackage loaded;
        QString error;
        if (CcPackage::load(packageFile, &loaded, &error)) {
            job->setPackage(loaded);
        } else {
            qCInfo(lcFirmware) << "remembered package" << packageFile << "not loaded:" << error;
        }
    }
    packageEdit->setText(nativePath(job->packagePath()));
    // Last session's role is shown, never confirmed: the board plugged in now may be another one
    // (REQUIREMENTS: "the user confirms, never guess silently"). Update MCU / Set role wait for a
    // Read (which pre-selects the reported role) or a click on a radio in this session.
    rememberedRole = FirmwareJob::roleFromText(QSettings().value(kRoleSettingsKey).toString());
    job->setRole(0);
    showRole(rememberedRole);   // logged by the constructor, after the tool paths
    // (v0.8.0) The board profile box as it was left (the setting only knows valid ids).
    const int profileIndex = profileCombo->findData(s.firmwareProfile());
    profileCombo->setCurrentIndex(profileIndex >= 0 ? profileIndex : 0);
    job->setProfileOverride(selectedProfileOverride());
    // (v0.8.0) Verify after writing as it was left (default off: the daily update stays as fast).
    verifyCheckBox->setChecked(s.firmwareVerifyAfterWrite());
    job->setVerifyAfterWrite(verifyCheckBox->isChecked());
    restoring = false;

    job->refreshImage();
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
    return d->job->isBusy();
}

QString FirmwareTab::persistentStatusMessage() const
{
    return d->job->persistentStatusMessage();
}

QString FirmwareTab::currentOperation() const
{
    return d->job->currentOperation();
}

FirmwareJob* FirmwareTab::firmwareJob() const { return d->job; }
ToolRunner* FirmwareTab::toolRunner() const { return d->job->toolRunner(); }
SocFlasher* FirmwareTab::socFlasher() const { return d->job->socFlasher(); }
McuLink* FirmwareTab::mcuLink() const { return d->job->mcuLink(); }
McuUpdater* FirmwareTab::mcuUpdater() const { return d->job->mcuUpdater(); }
ImagePuller* FirmwareTab::imagePuller() const { return d->job->imagePuller(); }
SshConnection* FirmwareTab::sshConnection() const { return d->job->sshConnection(); }

void FirmwareTab::setToolPaths(const QString& upgradeTool, const QString& adb, const QString& driverInstaller)
{
    d->job->setToolPaths(upgradeTool, adb);
    d->driverInstallerPath = driverInstaller;
    d->toolPathsPinned = true;
    qCInfo(lcFirmware) << "tool paths set: upgrade_tool" << upgradeTool << "adb" << adb;
    d->runFirmwareInfo(imagePath());   // the image line and the partitions depend on upgrade_tool
    d->job->planPartitions();
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
    d->job->setRemoteFolder(trimmed);
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
    d->job->setCacheDirectory(clean);
    d->job->refreshImage();
}

QString FirmwareTab::imagePath() const
{
    return d->job->imagePath();
}

void FirmwareTab::setImagePath(const QString& path)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    QString message;
    if (!d->job->setImagePath(clean, &message)) {
        emit statusMessage(message, kStatusLongMs);   // the job logged it
        return;
    }
    AppSettings::instance().setFirmwareLocalImage(clean);   // "" = back to the cache's image
}

QString FirmwareTab::packagePath() const
{
    return d->job->packagePath();
}

bool FirmwareTab::setPackagePath(const QString& path)
{
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    if (clean.isEmpty()) {
        d->packageProblem.clear();
        d->job->setPackagePath(QString());
        AppSettings::instance().setFirmwarePackagePath(QString());
        d->packageEdit->clear();
        d->renderPackage();
        d->updateControls();
        return false;
    }
    QString message;
    if (!d->job->setPackagePath(clean, &message)) {
        emit statusMessage(message, kStatusLongMs);   // the job logged it
        d->packageProblem = message;
        d->packageEdit->setText(nativePath(d->job->packagePath()));   // back to the accepted one
        d->renderPackage();
        return false;
    }
    d->packageProblem.clear();
    AppSettings::instance().setFirmwarePackagePath(clean);
    d->packageEdit->setText(nativePath(clean));
    d->renderPackage();
    d->updateControls();
    return true;
}

CcPackage FirmwareTab::package() const
{
    return d->job->package();
}

quint32 FirmwareTab::selectedRole() const
{
    return d->job->role();
}

void FirmwareTab::setSelectedRole(quint32 role)
{
    const quint32 valid = (role == CcProtocol::RoleRx || role == CcProtocol::RoleTx) ? role : 0;
    d->settingRole = true;
    d->job->setRole(valid);
    d->settingRole = false;
    d->showRole(valid);
    if (!d->restoring) {
        QSettings().setValue(kRoleSettingsKey, FirmwareJob::roleText(valid));
    }
    d->updateControls();
}

QString FirmwareTab::runningMcuVersion() const
{
    return d->job->runningMcuVersion();
}

QString FirmwareTab::runningMcuRole() const
{
    return d->job->runningMcuRole();
}

QComboBox* FirmwareTab::sourceCombo() const { return d->sourceCombo; }
QLineEdit* FirmwareTab::remoteFolderEdit() const { return d->remoteFolderEdit; }
QLineEdit* FirmwareTab::cacheDirEdit() const { return d->cacheDirEdit; }
QPushButton* FirmwareTab::pullButton() const { return d->pullButton; }
QPushButton* FirmwareTab::localImageButton() const { return d->localImageButton; }
QLabel* FirmwareTab::imageInfoLabel() const { return d->imageInfoLabel; }
QPushButton* FirmwareTab::rollbackButton() const { return d->rollbackButton; }
QLabel* FirmwareTab::socStatusLabel() const { return d->socStatusLabel; }
QComboBox* FirmwareTab::profileCombo() const { return d->profileCombo; }
QLabel* FirmwareTab::profileLabel() const { return d->profileLabel; }
QCheckBox* FirmwareTab::verifyCheckBox() const { return d->verifyCheckBox; }
QLabel* FirmwareTab::socImageLabel() const { return d->socImageLabel; }
QTableWidget* FirmwareTab::partitionTable() const { return d->partitionTable; }
QPushButton* FirmwareTab::partitionsAllButton() const { return d->partitionsAllButton; }
QPushButton* FirmwareTab::partitionsNoneButton() const { return d->partitionsNoneButton; }
QLabel* FirmwareTab::partitionsLabel() const { return d->partitionsLabel; }

QString FirmwareTab::flashLayout() const
{
    return d->job->flashLayout();
}

QStringList FirmwareTab::selectedPartitions() const
{
    QStringList names;
    for (int row = 0; row < d->partitionTable->rowCount(); ++row) {
        const QTableWidgetItem* item = d->partitionTable->item(row, ColCheck);
        if (item && (item->flags() & Qt::ItemIsEnabled) && item->checkState() == Qt::Checked) {
            names << item->data(Qt::UserRole).toString();
        }
    }
    return names;
}

void FirmwareTab::setSelectedPartitions(const QStringList& names)
{
    d->job->setSelectedPartitions(names);   // the table follows partitionsChanged()
}

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
    job->setSourceTarget(key);
    job->setRemoteFolder(q->remoteFolder());
    if (!job->isBusy() && job->sshConnection()->state() != Transport::State::Disconnected) {
        job->disconnectSource();   // a connection to the previous target is of no use any more
    }
}

void FirmwareTab::Private::syncJob()
{
    // The fields as they are now (the job takes the profile key, not the combo's text) and the
    // Preferences an operation reads when it starts; the job reads no settings itself.
    job->setSourceTarget(targetKey());
    job->setRemoteFolder(q->remoteFolder());
    job->setCacheDirectory(q->cacheDirectory());
    const AppSettings& settings = AppSettings::instance();
    FirmwareJob::SshDefaults defaults;
    defaults.knownHostsFile = settings.sshKnownHostsFile();
    defaults.identityFile = settings.sshDefaultIdentityFile();
    defaults.terminalType = settings.sshDefaultTerminalType();
    defaults.keepAliveSeconds = settings.sshDefaultKeepAliveSeconds();
    job->setSshDefaults(defaults);
    job->setGitDescribe(settings.firmwareGitDescribe());
    job->setExtraFiles(settings.firmwareExtraFiles());
    job->setProfileOverride(selectedProfileOverride());   // (v0.8.0) the Board profile box
    job->setVerifyAfterWrite(verifyCheckBox->isChecked());   // (v0.8.0) Flash SoC and Pull & Flash alike
}

void FirmwareTab::Private::renderImage(bool reread)
{
    if (reread) {
        sfiImage.clear();   // same path, new content: describe it again
    }
    const QString image = job->imagePath();
    const QString cache = q->cacheDirectory();
    QString text;
    if (image.isEmpty()) {
        text = FirmwareTab::tr("no image yet: pull one from the build machine or use a local image");
    } else {
        const QFileInfo info(image);
        if (!job->localImage().isEmpty()) {
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
    renderImageDetails();
    if (image != sfiImage) {
        runFirmwareInfo(image);
    }
    updateControls();   // the job plans the image right after (partitionsChanged())
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
    } else if (job->upgradeToolPath().isEmpty()) {
        sfiText = FirmwareTab::tr("upgrade_tool not found: no image details");
    } else {
        sfiText = FirmwareTab::tr("reading the image details...");
        ToolInvocation* invocation = job->socFlasher()->upgradeTool()->firmwareInfo(image);
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
// Partitions (v0.5.1): the table renders FirmwareJob's plan and selection
// =======================================================================================

void FirmwareTab::Private::renderPartitions()
{
    const FirmwareJob::PlanState planState = job->planState();
    const QList<PartitionImage> rows = job->partitions();
    {
        const QSignalBlocker blocker(partitionTable);   // checks set here are no user toggles
        partitionTable->setRowCount(0);
        if (planState == FirmwareJob::PlanState::Ready) {
            const bool known = job->layoutKnown();
            const QStringList selected = job->selectedPartitions();
            partitionTable->setRowCount(static_cast<int>(rows.size()));
            for (int i = 0; i < rows.size(); ++i) {
                const PartitionImage& row = rows.at(i);
                // Disabled and unchecked, with the reason as the tooltip: not in the layout, no
                // file, too big (or no layout at all).
                const QString problem = FirmwareJob::partitionProblem(row, known);
                const bool enabled = problem.isEmpty();
                const Qt::ItemFlags flags = enabled ? Qt::ItemFlags(Qt::ItemIsEnabled) : Qt::ItemFlags(Qt::NoItemFlags);
                auto* check = new QTableWidgetItem;
                check->setFlags(enabled ? Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable)
                                        : Qt::ItemFlags(Qt::NoItemFlags));
                check->setCheckState(enabled && selected.contains(row.name) ? Qt::Checked : Qt::Unchecked);
                check->setData(Qt::UserRole, row.name);
                const QStringList texts{
                    row.name,
                    QFileInfo(row.file).fileName(),
                    row.inLayout ? QStringLiteral("0x%1").arg(row.offset, 0, 16) : QString(),
                    row.inLayout ? layoutSizeText(row.size) : QString(),
                    row.fileSize >= 0 ? QLocale().toString(row.fileSize) : FirmwareTab::tr("missing")};
                partitionTable->setItem(i, ColCheck, check);
                for (int column = ColName; column < PartitionColumnCount; ++column) {
                    auto* item = new QTableWidgetItem(texts.at(column - ColName));
                    item->setFlags(flags);
                    if (column >= ColOffset) {
                        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    }
                    partitionTable->setItem(i, column, item);
                }
                const QString tip = problem.isEmpty() ? nativePath(row.file) : problem;
                for (int column = 0; column < PartitionColumnCount; ++column) {
                    partitionTable->item(i, column)->setToolTip(tip);
                }
            }
        }
    }

    const QString boardLayout = job->boardLayout();
    const QList<MtdPartition> boardParts = RockchipTool::parseMtdParts(boardLayout);
    const bool usingImageLayout = job->usingImageLayout();
    const bool imageDiffersFromBoard =
        usingImageLayout && !boardParts.isEmpty() && boardParts != RockchipTool::parseMtdParts(job->imageLayout());
    QString text;
    bool failed = false;
    switch (planState) {
    case FirmwareJob::PlanState::NoImage:
        text = FirmwareTab::tr("no image selected");
        break;
    case FirmwareJob::PlanState::Planning:
        text = FirmwareTab::tr("unpacking the image to list its partitions...");
        break;
    case FirmwareJob::PlanState::Failed:
        text = FirmwareTab::tr("cannot list the partitions: %1").arg(job->planMessage());
        failed = true;
        break;
    case FirmwareJob::PlanState::Ready:
        if (!job->layoutKnown()) {
            text = FirmwareTab::tr("partition layout unknown: connect the board running once");
        } else {
            const QStringList checked = q->selectedPartitions();
            qint64 bytes = 0;
            for (const PartitionImage& row : rows) {
                if (checked.contains(row.name) && row.fileSize > 0) {
                    bytes += row.fileSize;
                }
            }
            // Two plain strings instead of a %n plural: "1 partition" must read right without a translator.
            text = rows.size() == 1
                       ? FirmwareTab::tr("1 partition, %1 selected (%2)").arg(checked.size()).arg(formatSize(bytes))
                       : FirmwareTab::tr("%1 partitions, %2 selected (%3)")
                             .arg(rows.size())
                             .arg(checked.size())
                             .arg(formatSize(bytes));
            // Where the offsets come from: the image's env.img (the layout it was built for) or the board.
            text += QStringLiteral(" - ") + (!usingImageLayout ? FirmwareTab::tr("layout from the board")
                                             : imageDiffersFromBoard ? FirmwareTab::tr("layout from the image (the board's differs)")
                                                                     : FirmwareTab::tr("layout from the image"));
        }
        break;
    }
    partitionsLabel->setText(text);
    const QString layout = job->flashLayout();
    QString tip = text;
    if (usingImageLayout) {
        tip = FirmwareTab::tr("Flash layout from the image's env.img: %1").arg(layout);
        if (imageDiffersFromBoard) {
            tip += QLatin1Char('\n') + FirmwareTab::tr("The board's layout: %1").arg(boardLayout);
        }
    } else if (!layout.isEmpty()) {
        tip = FirmwareTab::tr("Flash layout from the board: %1").arg(layout);
    }
    partitionsLabel->setToolTip(tip);
    QPalette palette = partitionsLabel->palette();
    palette.setColor(QPalette::WindowText, failed ? kErrorText : q->palette().color(QPalette::WindowText));
    partitionsLabel->setPalette(palette);
    updateControls();
}

void FirmwareTab::Private::onPartitionItemChanged(QTableWidgetItem* item)
{
    if (!item || item->column() != ColCheck) {
        return;
    }
    job->setSelectedPartitions(q->selectedPartitions());   // re-renders the label's count and size
}

// =======================================================================================
// Rendering / enable states
// =======================================================================================

// ---- Board profiles (v0.8.0) ------------------------------------------------------------------

QString FirmwareTab::Private::selectedProfileOverride() const
{
    return profileCombo->currentData().toString();
}

void FirmwareTab::Private::renderProfile()
{
    const ProfileDetection profile = job->boardProfile();
    if (profile.known()) {
        profileLabel->setText(FirmwareTab::tr("Profile: %1 - %2").arg(profile.profile.name, profile.sourceText()));
        profileLabel->setToolTip(profile.evidence);
    } else {
        profileLabel->setText(FirmwareTab::tr("Profile: unknown"));
        profileLabel->setToolTip(QString());
    }
}

/// The image's RKFW header and profile in the image lines' tooltips (the job reads the header).
void FirmwareTab::Private::renderImageDetails()
{
    const QString image = job->imagePath();
    if (image.isEmpty()) {
        socImageLabel->setToolTip(QString());
        return;
    }
    const RkfwHeader header = job->imageHeader();
    const ProfileDetection profile = job->imageProfile();
    QStringList lines;
    if (header.valid) {
        lines << FirmwareTab::tr("RKFW header: chip tag %1, version %2, built %3")
                     .arg(header.chipTag, header.version,
                          header.buildTime.isValid() ? header.buildTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("?"));
    } else {
        lines << FirmwareTab::tr("RKFW header unreadable");
    }
    lines << (profile.known() ? FirmwareTab::tr("Image profile: %1 - %2").arg(profile.profile.name, profile.sourceText())
                              : FirmwareTab::tr("Image profile: unknown"));
    socImageLabel->setToolTip(lines.join(QLatin1Char('\n')));
    imageInfoLabel->setToolTip(nativePath(image) + QLatin1Char('\n') + lines.join(QLatin1Char('\n')));
}

/// A new image planned, or a board profile that became known, while every row is checked: the
/// profile's daily partitions are the default checks (the Node: rootfs; the stacked board: boot).
/// Once per image and profile - (fix4, m3) the key holds both, and boardProfileChanged() applies
/// it too: the image is usually planned at start-up and the profile arrives with a status seconds
/// later -; the user's own checks (anything but "all") are left alone; an unknown profile keeps
/// every row. Never while an operation runs: the selection is the running flash's input.
void FirmwareTab::Private::applyDailyPartitions()
{
    if (job->isBusy() || job->planState() != FirmwareJob::PlanState::Ready) {
        return;
    }
    const QString image = job->plannedImage();
    const ProfileDetection profile = job->boardProfile();
    const QString key = image + QLatin1Char('\n') + QString::number(QFileInfo(image).lastModified().toMSecsSinceEpoch())
                        + QLatin1Char('\n') + profile.profile.id;
    if (key == dailyImageKey) {
        return;
    }
    dailyImageKey = key;
    if (!profile.known() || !job->allPartitionsSelected()) {
        return;
    }
    const bool known = job->layoutKnown();
    QStringList names;
    for (const PartitionImage& row : job->partitions()) {
        if (profile.profile.dailyPartitions.contains(row.name) && FirmwareJob::partitionProblem(row, known).isEmpty()) {
            names << row.name;
        }
    }
    if (!names.isEmpty()) {
        job->setSelectedPartitions(names);
    }
}

/// One rule's question: its refusal text up to the flag's sentence, then `question`; Cancel is
/// the default. Closed by the end of the operation (busyChanged(false)).
bool FirmwareTab::Private::askProfileQuestion(const QString& refusal, const QString& flag, const QString& question,
                                              const QString& accept)
{
    QString message = refusal;
    const qsizetype at = message.indexOf(flag);
    if (at > 0) {
        message = message.left(at).trimmed();
        while (message.endsWith(QLatin1Char(';')) || message.endsWith(QLatin1Char('-'))) {
            message.chop(1);
            message = message.trimmed();
        }
    }
    message += QStringLiteral("\n\n") + question;
    QMessageBox box(QMessageBox::Question, FirmwareTab::tr("Firmware Update"), message, QMessageBox::NoButton, q->window());
    QPushButton* yes = box.addButton(accept, QMessageBox::AcceptRole);
    QPushButton* no = box.addButton(FirmwareTab::tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(no);
    box.setEscapeButton(no);
    QPointer<FirmwareTab> self(q);
    profileBox = &box;
    box.exec();
    if (!self) {
        return false;   // the tab (and its job) went away meanwhile
    }
    profileBox = nullptr;
    return box.clickedButton() == yes;
}

void FirmwareTab::Private::renderDeviceStatus()
{
    if (!job->hasStatus()) {
        socStatusLabel->setText(FirmwareTab::tr("not checked yet"));
        installDriversButton->setVisible(false);
        return;
    }
    const SocFlasher::DeviceStatus deviceStatus = job->lastStatus();
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
    if (job->boardInfo().isEmpty()) {
        socStatusLabel->setToolTip(deviceStatus.detail);
    }
    installDriversButton->setVisible(noDriver);
}

void FirmwareTab::Private::renderPackage()
{
    QPalette palette = packageInfoLabel->palette();
    const CcPackage mcuPackage = job->package();
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
    const QString version = job->runningMcuVersion();
    if (version.isEmpty()) {
        mcuStatusLabel->setText(FirmwareTab::tr("unknown"));
    } else {
        mcuStatusLabel->setText(QStringLiteral("%1 %2").arg(version, job->runningMcuRole()).trimmed());
    }
}

void FirmwareTab::Private::showRole(quint32 role)
{
    const QSignalBlocker rxBlocker(roleRxRadio);
    const QSignalBlocker txBlocker(roleTxRadio);
    roleGroup->setExclusive(false);   // an exclusive group cannot show "none chosen"
    roleRxRadio->setChecked(role == CcProtocol::RoleRx);
    roleTxRadio->setChecked(role == CcProtocol::RoleTx);
    roleGroup->setExclusive(true);
}

void FirmwareTab::Private::updateControls()
{
    const bool idle = !job->isBusy();
    for (QWidget* widget : {static_cast<QWidget*>(sourceCombo), static_cast<QWidget*>(profilesButton),
                            static_cast<QWidget*>(remoteFolderEdit), static_cast<QWidget*>(cacheDirEdit),
                            static_cast<QWidget*>(cacheDirButton), static_cast<QWidget*>(pullButton),
                            static_cast<QWidget*>(localImageButton), static_cast<QWidget*>(packageEdit),
                            static_cast<QWidget*>(packageButton), static_cast<QWidget*>(readMcuButton),
                            static_cast<QWidget*>(roleRxRadio), static_cast<QWidget*>(roleTxRadio),
                            static_cast<QWidget*>(pullAndFlashButton), static_cast<QWidget*>(installDriversButton),
                            static_cast<QWidget*>(verifyCheckBox), static_cast<QWidget*>(profileCombo)}) {
        // (fix4, M2) profileCombo too: a change reaches the running flasher at once (the override).
        widget->setEnabled(idle);
    }
    rollbackButton->setEnabled(idle && hasPreviousImage);
    flashSocButton->setEnabled(idle && !job->imagePath().isEmpty());
    bool checkable = false;
    for (int row = 0; row < partitionTable->rowCount() && !checkable; ++row) {
        const QTableWidgetItem* item = partitionTable->item(row, ColCheck);
        checkable = item && (item->flags() & Qt::ItemIsEnabled);
    }
    partitionTable->setEnabled(idle);
    partitionsAllButton->setEnabled(idle && checkable);
    partitionsNoneButton->setEnabled(idle && checkable);
    // The role is what the user confirms: never guessed, so no update without one.
    updateMcuButton->setEnabled(idle && job->package().isValid() && job->role() != 0);
    setRoleButton->setEnabled(idle && job->role() != 0);
    cancelButton->setEnabled(job->isCancellable());
}

void FirmwareTab::Private::updateStatusTimer()
{
    if (refreshMs > 0 && q->isVisible() && !job->isBusy()) {
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

void FirmwareTab::Private::logToolPaths()
{
    const AppSettings& s = AppSettings::instance();
    q->appendLog(FirmwareTab::tr("upgrade_tool: %1").arg(ToolLocator::describe(ToolLocator::Tool::UpgradeTool, s.firmwareUpgradeToolPath())));
    q->appendLog(FirmwareTab::tr("adb: %1").arg(ToolLocator::describe(ToolLocator::Tool::Adb, s.firmwareAdbPath())));
}

// =======================================================================================
// The job's reports and questions
// =======================================================================================

void FirmwareTab::Private::onProgress(const QString& stage, int percent)
{
    if (percent == FirmwareJob::kProgressTextOnly) {
        progressLabel->setText(stage);
        return;
    }
    progressShowsReady = false;
    if (percent == FirmwareJob::kProgressBusy) {
        progressBar->setRange(0, 0);   // indeterminate
    } else {
        if (progressBar->maximum() != 100) {
            progressBar->setRange(0, 100);
        }
        progressBar->setValue(qBound(0, percent, 100));
    }
    progressLabel->setText(stage);
}

void FirmwareTab::Private::onStepFinished(const QString& step, bool ok, const QString& message)
{
    if (step == FirmwareJob::kOpPull && ok) {
        // The freshly pulled (or unchanged) cache image is what Flash SoC uses from now on.
        AppSettings::instance().setFirmwareLocalImage(QString());
    }
    if (step == FirmwareJob::kOpFlash && ok && !job->boardInfo().isEmpty()) {
        socStatusLabel->setToolTip(job->boardInfo());
    }
    if (job->currentOperation() == FirmwareJob::kOpPullFlash) {
        emit q->operationFinished(step, ok, message);   // one per step of a Pull & Flash, then the final one
    }
}

void FirmwareTab::Private::onOperationFinished(const QString& op, bool ok, const QString& message)
{
    if (progressBar->maximum() == 0) {
        progressBar->setRange(0, 100);
        progressBar->setValue(ok ? 100 : 0);
    } else if (ok) {
        progressBar->setValue(100);
    }
    const QString text = FirmwareJob::resultText(op, ok, message);   // logged by the job
    progressLabel->setText(text);
    emit q->statusMessage(text, kStatusLongMs);
    emit q->operationFinished(op, ok, message);
}

FirmwareJob::HostKeyDecision FirmwareTab::Private::askHostKey(const SshConnection::HostKeyInfo& info)
{
    // As SessionWidget::onHostKeyVerificationRequired(): the dialog lives on the heap and the tab
    // is tracked, because either may go away during the nested event loop.
    QPointer<FirmwareTab> self(q);
    QPointer<HostKeyDialog> dialog(new HostKeyDialog(info, q->window()));
    const int result = dialog->exec();
    FirmwareJob::HostKeyDecision decision;
    if (!self || !dialog) {
        delete dialog;
        return decision;
    }
    decision.accept = (result == QDialog::Accepted);
    decision.remember = decision.accept && dialog->remember();
    delete dialog;
    return decision;
}

std::optional<FirmwareJob::AuthAnswer> FirmwareTab::Private::askAuthPrompt(const SshConnection::AuthPrompt& prompt)
{
    QPointer<FirmwareTab> self(q);
    QPointer<AuthPromptDialog> dialog(new AuthPromptDialog(prompt, q->window()));
    const int result = dialog->exec();
    if (!self || !dialog || result != QDialog::Accepted) {
        delete dialog;
        return std::nullopt;
    }
    FirmwareJob::AuthAnswer answer;
    answer.response = dialog->response();
    answer.remember = dialog->remember();
    delete dialog;
    return answer;
}

FirmwareJob::DownloadModeAnswer FirmwareTab::Private::askDownloadMode()
{
    FirmwareJob::DownloadModeAnswer answer;
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
        return answer;   // nothing to ask: the job shows the MASKROM instructions
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
    downloadBox = &box;   // the flasher leaving WaitingForLoader closes it (wireJob())
    const int result = box.exec();
    answer.asked = true;   // the job checks whether the flasher still waits before anything is sent
    if (!self) {
        return answer;   // the tab (and its job) went away meanwhile
    }
    downloadBox = nullptr;
    QPointer<SessionWidget> chosen;
    if (result == QMessageBox::Yes) {
        chosen = connected.first();
    } else {
        const qsizetype index = extra.indexOf(box.clickedButton());
        if (index >= 0) {
            chosen = connected.at(index + 1);
        }
    }
    if (chosen) {
        answer.send = [chosen]() -> QString {
            if (!chosen || !chosen->isConnected()) {
                return QString();   // gone meanwhile: the MASKROM instructions instead
            }
            chosen->sendBytes(QByteArrayLiteral("reboot loader\r"));
            return chosen->title();
        };
    }
    return answer;
}

// =======================================================================================
// Public slots
// =======================================================================================

void FirmwareTab::pull()
{
    d->syncJob();
    d->job->pull();
}

void FirmwareTab::flashSoc()
{
    d->syncJob();
    d->job->flashSoc();
}

void FirmwareTab::updateMcu()
{
    d->syncJob();
    d->job->updateMcu();
}

void FirmwareTab::setMcuRole()
{
    d->syncJob();
    d->job->setMcuRole();
}

void FirmwareTab::readMcuInfo()
{
    d->syncJob();
    d->job->readMcuInfo();
}

void FirmwareTab::pullAndFlash()
{
    d->syncJob();
    d->job->pullAndFlash();
}

void FirmwareTab::cancel()
{
    d->job->cancel();
}

void FirmwareTab::rollBack()
{
    if (isBusy()) {
        d->job->rollBack();   // refused with "Busy: ..." in the status bar
        return;
    }
    d->job->setCacheDirectory(cacheDirectory());
    QString message;
    if (!d->job->rollBack(&message)) {
        emit statusMessage(message, kStatusLongMs);   // the job logged it
        return;
    }
    AppSettings::instance().setFirmwareLocalImage(QString());
    emit statusMessage(message, kStatusShortMs);
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
    const QString current = d->job->packagePath();
    const QString start = current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose a CC1310 Firmware Package"), start,
                                                      tr("CC1310 firmware packages (*.pkg);;All files (*)"));
    if (!file.isEmpty()) {
        setPackagePath(file);
    }
}

void FirmwareTab::refreshStatus()
{
    d->job->refreshStatus();   // nothing while busy: upgrade_tool / adb belong to the running operation
}

void FirmwareTab::planPartitions()
{
    d->job->planPartitions();
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
    ToolInvocation* invocation = d->job->toolRunner()->run(installer, {}, options);
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
    d->appendLines(line, /*mirror=*/true);
}

void FirmwareTab::Private::appendLines(const QString& line, bool mirror)
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
        logView->appendPlainText(stamped);
        if (mirror) {
            qCInfo(lcFirmware).noquote() << trimmed;
        }
        emit q->logLineAdded(stamped);
    }
    lastLogWasProgress = false;
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
        const bool changed = upgradeTool != d->job->upgradeToolPath() || adb != d->job->adbPath()
                             || d->logView->document()->isEmpty();
        d->job->setToolPaths(upgradeTool, adb);
        d->driverInstallerPath = ToolLocator::locate(ToolLocator::Tool::DriverInstaller);
        if (changed) {
            d->logToolPaths();
            d->runFirmwareInfo(imagePath());
            d->job->planPartitions();
        }
    }
    d->job->setLoaderWaitSeconds(s.firmwareLoaderWaitSeconds());
    d->job->setAdbReturnSeconds(s.firmwareAdbReturnSeconds());
    if (d->verifyCheckBox->isChecked() != s.firmwareVerifyAfterWrite()) {
        d->verifyCheckBox->setChecked(s.firmwareVerifyAfterWrite());   // (v0.8.0) the setting changed elsewhere
    }
    const QString cache = s.firmwareCacheDirectory();
    if (cache != cacheDirectory()) {
        d->cacheDirEdit->setText(nativePath(cache));
        d->job->setCacheDirectory(cache);
        d->job->refreshImage();
    }
}

void FirmwareTab::disconnectSource()
{
    d->job->disconnectSource();
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
