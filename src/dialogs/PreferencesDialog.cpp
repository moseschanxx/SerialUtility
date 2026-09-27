#include "dialogs/PreferencesDialog.h"
#include "ui_PreferencesDialog.h"

#include <QAbstractButton>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QHeaderView>
#include <QIntValidator>
#include <QItemSelectionModel>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSerialPort>
#include <QStandardPaths>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <utility>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/LineEnding.h"
#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"
#include "core/SessionLogger.h"
#include "ssh/SecretStore.h"
#include "ssh/SshConnection.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalTheme.h"

namespace {

// Tab indices as laid out in PreferencesDialog.ui.
enum Page { PageTerminal = 0, PageInput, PageConnection, PageSsh, PageKeyboard, PageLogging, PageGeneral };

// Columns of the Keyboard page's table.
enum ShortcutColumn { ColumnAction = 0, ColumnShortcut, ColumnDefault, ShortcutColumnCount };

const QLatin1String kDefaultSshTerminalType("xterm-256color");
constexpr int kDefaultSshKeepAlive = 30;
constexpr int kDefaultAutoBaudSampleMs = 1500;

/// "115200, 1500000, ..." for the candidates edit and its tooltip.
QString joinBaudRates(const QList<qint32>& rates)
{
    QStringList parts;
    parts.reserve(rates.size());
    for (qint32 rate : rates) {
        parts.append(QString::number(rate));
    }
    return parts.join(QStringLiteral(", "));
}

/// A key sequence as shown in the table (native text, empty when there is none).
QString sequenceText(const QKeySequence& sequence)
{
    return sequence.isEmpty() ? QString() : sequence.toString(QKeySequence::NativeText);
}

/// The first key stroke of what a QKeySequenceEdit produced: shortcuts are single chords here.
QKeySequence firstChord(const QKeySequence& sequence)
{
    return sequence.count() > 1 ? QKeySequence(sequence[0]) : sequence;
}

/// True when a single chord can serve as an application shortcut: Ctrl, Alt or Meta is held,
/// or the key is an F-key (F1..F35, with or without Shift). Anything else - a bare letter,
/// digit, Enter, Space, Backspace, Tab, Esc, an arrow or navigation key, Shift+<those> - is text
/// or a control key of the connected terminal; reserving it would take it away from the shell
/// (a plain "a" would trigger the action instead of being typed).
bool isApplicationShortcutChord(const QKeySequence& chord)
{
    if (chord.isEmpty() || chord.count() != 1) {
        return false;
    }
    const QKeyCombination combo = chord[0];
    const int key = combo.key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        return true;
    }
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    return mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::AltModifier) || mods.testFlag(Qt::MetaModifier);
}

/// The directory a file browser starts in: the file's own directory when it exists, else ~/.ssh
/// when that exists, else the home directory.
QString sshBrowseStart(const QString& currentPath)
{
    const QString current = currentPath.trimmed();
    if (!current.isEmpty()) {
        const QFileInfo info(current);
        if (info.dir().exists()) {
            return info.dir().absolutePath();
        }
    }
    const QString sshDir = QDir::homePath() + QStringLiteral("/.ssh");
    return QDir(sshDir).exists() ? sshDir : QDir::homePath();
}

/// Select the item whose user data equals `value`; falls back to the first item.
void selectByData(QComboBox* combo, const QVariant& value)
{
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

/// Like selectByData(), but when `value` is not in the combo, append it (display `text`)
/// and select it so an OK/Apply round trip never rewrites a stored value the user did not touch.
void selectOrInsert(QComboBox* combo, const QString& text, const QString& value)
{
    int index = combo->findData(value);
    if (index < 0 && !value.isEmpty()) {
        combo->addItem(text, value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

void selectBaud(QComboBox* combo, qint32 baud)
{
    const QString text = QString::number(baud);
    const int index = combo->findText(text);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    } else {
        combo->setEditText(text);
    }
}

constexpr int kAutoBaudIndex = 0;       ///< the "Auto" item of the default baud combo (item data 0)
const QLatin1String kAutoWord("Auto");  ///< accepted whatever the UI language

/// "Auto" (any case) or the combo's translated item text.
bool isAutoBaudText(const QComboBox* combo, const QString& text)
{
    const QString typed = text.trimmed();
    return !typed.isEmpty() && (typed.compare(kAutoWord, Qt::CaseInsensitive) == 0 ||
                                typed.compare(combo->itemText(kAutoBaudIndex), Qt::CaseInsensitive) == 0);
}

/// The default baud combo's validator: the baud range of a QIntValidator plus the word "Auto"
/// (the English word and the item's translated text, case-insensitively; a prefix of either is
/// Intermediate so it can be typed). The same rule as the ConnectionBar's editor.
class DefaultBaudValidator : public QIntValidator
{
public:
    using QIntValidator::QIntValidator;

    void setAutoText(const QString& text) { m_autoText = text; }

    State validate(QString& input, int& pos) const override
    {
        const QString typed = input.trimmed();
        if (!typed.isEmpty() && !typed.at(0).isDigit()) {
            State best = Invalid;
            for (const QString& word : {m_autoText, QString(kAutoWord)}) {
                if (word.compare(typed, Qt::CaseInsensitive) == 0) {
                    return Acceptable;
                }
                if (word.startsWith(typed, Qt::CaseInsensitive)) {
                    best = Intermediate;
                }
            }
            return best;
        }
        return QIntValidator::validate(input, pos);
    }

private:
    QString m_autoText = kAutoWord;
};

/// Select "Auto" or the fixed rate of `serial` in the default baud combo.
void selectDefaultBaud(QComboBox* combo, const SerialSettings& serial)
{
    if (serial.autoBaud) {
        combo->setCurrentIndex(kAutoBaudIndex);
        combo->setEditText(combo->itemText(kAutoBaudIndex));   // in case "Auto" was already current
    } else {
        selectBaud(combo, serial.baudRate);
    }
}

} // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::PreferencesDialog)
    , m_font(AppSettings::defaultTerminalFont())
{
    ui->setupUi(this);
    setWindowTitle(tr("Preferences"));

    setupPages();

    connect(ui->fontButton, &QPushButton::clicked, this, &PreferencesDialog::onFontButton);
    connect(ui->logDirBrowseButton, &QPushButton::clicked, this, &PreferencesDialog::onBrowseLogDir);
    connect(ui->sshKnownHostsBrowseButton, &QPushButton::clicked, this, &PreferencesDialog::onBrowseKnownHosts);
    connect(ui->sshIdentityBrowseButton, &QPushButton::clicked, this, &PreferencesDialog::onBrowseIdentityFile);
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, &PreferencesDialog::onButtonClicked);
    connect(ui->autoReconnectCheck, &QCheckBox::toggled, ui->reconnectIntervalSpin, &QWidget::setEnabled);
    connect(ui->defaultFlowCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto flow = ui->defaultFlowCombo->currentData().value<QSerialPort::FlowControl>();
        ui->rtsCheck->setEnabled(flow != QSerialPort::HardwareControl);
    });
    connect(ui->shortcutTable->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { onShortcutRowChanged(); });
    connect(ui->shortcutEdit, &QKeySequenceEdit::keySequenceChanged, this, &PreferencesDialog::onShortcutEdited);
    connect(ui->shortcutClearButton, &QPushButton::clicked, this, &PreferencesDialog::onShortcutClear);
    connect(ui->shortcutRestoreButton, &QPushButton::clicked, this, &PreferencesDialog::onShortcutRestore);

    loadFromSettings();
}

void PreferencesDialog::setShortcutEntries(const QList<ShortcutEntry>& entries)
{
    m_shortcutEntries = entries;
    loadShortcutRows();
}

void PreferencesDialog::setFixedShortcuts(const QList<ShortcutEntry>& fixed)
{
    m_fixedShortcuts.clear();
    for (const ShortcutEntry& entry : fixed) {
        if (!entry.defaultSequence.isEmpty()) {
            m_fixedShortcuts.append(entry);
        }
    }
    updateShortcutConflicts();
}

PreferencesDialog::~PreferencesDialog()
{
    delete ui;
}

void PreferencesDialog::setupPages()
{
    // ---- Terminal -------------------------------------------------------------------
    ui->themeCombo->clear();
    const QStringList themes = TerminalTheme::names();
    for (const QString& name : themes) {
        ui->themeCombo->addItem(TerminalTheme::displayName(name), name);
    }
    ui->scrollbackSpin->setRange(100, 1000000);

    // ---- Input ----------------------------------------------------------------------
    ui->enterSendsCombo->clear();
    const QList<LineEnding::Mode> modes = LineEnding::allModes();
    for (LineEnding::Mode mode : modes) {
        ui->enterSendsCombo->addItem(LineEnding::displayName(mode), static_cast<int>(mode));
    }
    ui->encodingCombo->clear();
    const QStringList encodings = AnsiParser::availableEncodings();
    for (const QString& encoding : encodings) {
        ui->encodingCombo->addItem(encoding, encoding);
    }

    // ---- Connection -----------------------------------------------------------------
    ui->defaultBaudCombo->clear();
    ui->defaultBaudCombo->setEditable(true);
    ui->defaultBaudCombo->setInsertPolicy(QComboBox::NoInsert);
    auto* baudValidator =
        new DefaultBaudValidator(SerialSettings::kMinBaudRate, SerialSettings::kMaxBaudRate, ui->defaultBaudCombo);
    baudValidator->setAutoText(tr("Auto"));
    ui->defaultBaudCombo->setValidator(baudValidator);
    // "Auto" first (item data 0, like the ConnectionBar): new sessions then start on the bar's
    // Auto item and detect the rate on connect.
    ui->defaultBaudCombo->addItem(tr("Auto"), 0);   // kAutoBaudIndex
    ui->defaultBaudCombo->setItemData(kAutoBaudIndex,
                                      tr("Detect the baud rate automatically when a session connects"), Qt::ToolTipRole);
    const QList<qint32> bauds = SerialSettings::standardBaudRates();
    for (qint32 baud : bauds) {
        ui->defaultBaudCombo->addItem(QString::number(baud), baud);
    }

    ui->defaultDataBitsCombo->clear();
    ui->defaultDataBitsCombo->addItem(QStringLiteral("5"), QVariant::fromValue(QSerialPort::Data5));
    ui->defaultDataBitsCombo->addItem(QStringLiteral("6"), QVariant::fromValue(QSerialPort::Data6));
    ui->defaultDataBitsCombo->addItem(QStringLiteral("7"), QVariant::fromValue(QSerialPort::Data7));
    ui->defaultDataBitsCombo->addItem(QStringLiteral("8"), QVariant::fromValue(QSerialPort::Data8));

    ui->defaultParityCombo->clear();
    ui->defaultParityCombo->addItem(tr("None"), QVariant::fromValue(QSerialPort::NoParity));
    ui->defaultParityCombo->addItem(tr("Even"), QVariant::fromValue(QSerialPort::EvenParity));
    ui->defaultParityCombo->addItem(tr("Odd"), QVariant::fromValue(QSerialPort::OddParity));
    ui->defaultParityCombo->addItem(tr("Space"), QVariant::fromValue(QSerialPort::SpaceParity));
    ui->defaultParityCombo->addItem(tr("Mark"), QVariant::fromValue(QSerialPort::MarkParity));

    ui->defaultStopBitsCombo->clear();
    ui->defaultStopBitsCombo->addItem(SerialSettings::stopBitsText(QSerialPort::OneStop),
                                      QVariant::fromValue(QSerialPort::OneStop));
    ui->defaultStopBitsCombo->addItem(SerialSettings::stopBitsText(QSerialPort::OneAndHalfStop),
                                      QVariant::fromValue(QSerialPort::OneAndHalfStop));
    ui->defaultStopBitsCombo->addItem(SerialSettings::stopBitsText(QSerialPort::TwoStop),
                                      QVariant::fromValue(QSerialPort::TwoStop));

    ui->defaultFlowCombo->clear();
    ui->defaultFlowCombo->addItem(SerialSettings::flowControlText(QSerialPort::NoFlowControl),
                                  QVariant::fromValue(QSerialPort::NoFlowControl));
    ui->defaultFlowCombo->addItem(SerialSettings::flowControlText(QSerialPort::HardwareControl),
                                  QVariant::fromValue(QSerialPort::HardwareControl));
    ui->defaultFlowCombo->addItem(SerialSettings::flowControlText(QSerialPort::SoftwareControl),
                                  QVariant::fromValue(QSerialPort::SoftwareControl));
    ui->reconnectIntervalSpin->setRange(200, 60000);
    // Automatic baud-rate detection: digits, commas and spaces only; the tooltip names the default.
    ui->autoBaudCandidatesEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9, ]*")), ui->autoBaudCandidatesEdit));
    ui->autoBaudCandidatesEdit->setToolTip(
        tr("Baud rates tried, in this order, when the baud rate is set to Auto or Session > Detect Baud Rate runs. "
           "Comma-separated. Default: %1")
            .arg(joinBaudRates(AppSettings::defaultAutoBaudCandidates())));
    ui->autoBaudSampleSpin->setRange(300, 10000);
    ui->autoBaudSampleSpin->setSuffix(tr(" ms"));

    // ---- Keyboard -------------------------------------------------------------------
    ui->shortcutTable->setColumnCount(ShortcutColumnCount);
    ui->shortcutTable->setHorizontalHeaderLabels({tr("Action"), tr("Shortcut"), tr("Default")});
    ui->shortcutTable->horizontalHeader()->setSectionResizeMode(ColumnAction, QHeaderView::Stretch);
    ui->shortcutTable->horizontalHeader()->setSectionResizeMode(ColumnShortcut, QHeaderView::ResizeToContents);
    ui->shortcutTable->horizontalHeader()->setSectionResizeMode(ColumnDefault, QHeaderView::ResizeToContents);
    ui->shortcutTable->horizontalHeader()->setStretchLastSection(false);
    ui->shortcutTable->verticalHeader()->setVisible(false);
    ui->shortcutTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->shortcutTable->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->shortcutTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->shortcutEdit->setMaximumSequenceLength(1);   // one key stroke per shortcut
    ui->shortcutEdit->setClearButtonEnabled(false);
    ui->shortcutEdit->setEnabled(false);
    ui->shortcutClearButton->setEnabled(false);
    ui->shortcutRestoreButton->setEnabled(false);
    ui->shortcutConflictLabel->setVisible(false);

    // ---- SSH ------------------------------------------------------------------------
    ui->sshKnownHostsEdit->setPlaceholderText(QDir::toNativeSeparators(SshConnection::defaultKnownHostsFile()));
    ui->sshTerminalTypeEdit->setPlaceholderText(kDefaultSshTerminalType);
    ui->sshKeepAliveSpin->setRange(0, 600);
    ui->sshKeepAliveSpin->setSuffix(tr(" s"));
    ui->sshKeepAliveSpin->setSpecialValueText(tr("Off"));
    ui->sshSecretsNoteLabel->setText(SecretStore::storageDescription());

    // ---- Logging --------------------------------------------------------------------
    ui->logFormatCombo->clear();
    ui->logFormatCombo->addItem(tr("Raw bytes (replayable capture)"),
                                SessionLogger::formatToString(SessionLogger::Format::Raw));
    ui->logFormatCombo->addItem(tr("Timestamped text"), SessionLogger::formatToString(SessionLogger::Format::Text));
    ui->logFormatCombo->addItem(tr("Hex dump"), SessionLogger::formatToString(SessionLogger::Format::HexDump));
    ui->logDirEdit->setPlaceholderText(QDir::toNativeSeparators(AppSettings::defaultLogDirectory()));

    ui->tabWidget->setCurrentIndex(PageTerminal);
    updateFontPreview();
}

void PreferencesDialog::loadFromSettings()
{
    const AppSettings& settings = AppSettings::instance();

    // Terminal
    m_font = settings.terminalFont();
    updateFontPreview();
    selectOrInsert(ui->themeCombo, TerminalTheme::displayName(settings.themeName()), settings.themeName());
    ui->scrollbackSpin->setValue(settings.scrollbackLines());
    ui->cursorBlinkCheck->setChecked(settings.cursorBlink());
    ui->bellCheck->setChecked(settings.bellEnabled());
    ui->implicitCrCheck->setChecked(settings.implicitCr());
    ui->pauseWhileSelectingCheck->setChecked(settings.pauseWhileSelecting());
    ui->rightClickPastesCheck->setChecked(settings.rightClickPastes());

    // Input
    selectByData(ui->enterSendsCombo, static_cast<int>(settings.enterSends()));
    ui->backspaceDeleteCheck->setChecked(settings.backspaceSendsDelete());
    ui->localEchoCheck->setChecked(settings.localEcho());
    selectOrInsert(ui->encodingCombo, settings.encoding(), settings.encoding());

    // Connection
    const SerialSettings serial = settings.defaultSerialSettings();
    selectDefaultBaud(ui->defaultBaudCombo, serial);
    selectByData(ui->defaultDataBitsCombo, QVariant::fromValue(serial.dataBits));
    selectByData(ui->defaultParityCombo, QVariant::fromValue(serial.parity));
    selectByData(ui->defaultStopBitsCombo, QVariant::fromValue(serial.stopBits));
    selectByData(ui->defaultFlowCombo, QVariant::fromValue(serial.flowControl));
    ui->dtrCheck->setChecked(serial.dtr);
    ui->rtsCheck->setChecked(serial.rts);
    ui->rtsCheck->setEnabled(serial.flowControl != QSerialPort::HardwareControl);
    ui->autoReconnectCheck->setChecked(settings.autoReconnect());
    ui->reconnectIntervalSpin->setValue(settings.reconnectIntervalMs());
    ui->reconnectIntervalSpin->setEnabled(settings.autoReconnect());
    ui->showSimulatedPortsCheck->setChecked(settings.showSimulatedPorts());
    setAutoBaudCandidatesText(settings.autoBaudCandidates());
    ui->autoBaudSampleSpin->setValue(settings.autoBaudSampleMs());
    ui->autoBaudWatchdogCheck->setChecked(settings.autoBaudWatchdog());

    // Keyboard (the rows come from setShortcutEntries(); their values from AppSettings)
    loadShortcutRows();

    // SSH (empty paths stay empty: the placeholder shows what "default" means)
    ui->sshKnownHostsEdit->setText(QDir::toNativeSeparators(settings.sshKnownHostsFile()));
    ui->sshIdentityEdit->setText(QDir::toNativeSeparators(settings.sshDefaultIdentityFile()));
    ui->sshTerminalTypeEdit->setText(settings.sshDefaultTerminalType());
    ui->sshKeepAliveSpin->setValue(settings.sshDefaultKeepAliveSeconds());

    // Logging
    ui->logDirEdit->setText(QDir::toNativeSeparators(settings.logDirectory()));
    ui->autoLogCheck->setChecked(settings.autoLog());
    selectByData(ui->logFormatCombo, settings.logFormat());
    ui->logIncludeTxCheck->setChecked(settings.logIncludeTx());

    // General
    ui->confirmCloseCheck->setChecked(settings.confirmCloseWhenConnected());
    ui->restoreSessionsCheck->setChecked(settings.restoreLastPorts());
}

void PreferencesDialog::saveToSettings()
{
    AppSettings& settings = AppSettings::instance();

    // Terminal
    settings.setTerminalFont(m_font);
    settings.setThemeName(ui->themeCombo->currentData().toString());
    settings.setScrollbackLines(ui->scrollbackSpin->value());
    settings.setCursorBlink(ui->cursorBlinkCheck->isChecked());
    settings.setBellEnabled(ui->bellCheck->isChecked());
    settings.setImplicitCr(ui->implicitCrCheck->isChecked());
    settings.setPauseWhileSelecting(ui->pauseWhileSelectingCheck->isChecked());
    settings.setRightClickPastes(ui->rightClickPastesCheck->isChecked());

    // Input
    settings.setEnterSends(static_cast<LineEnding::Mode>(ui->enterSendsCombo->currentData().toInt()));
    settings.setBackspaceSendsDelete(ui->backspaceDeleteCheck->isChecked());
    settings.setLocalEcho(ui->localEchoCheck->isChecked());
    settings.setEncoding(ui->encodingCombo->currentData().toString());

    // Connection
    SerialSettings serial = settings.defaultSerialSettings();
    const QString baudText = ui->defaultBaudCombo->currentText().trimmed();
    bool baudOk = false;
    const qint32 baud = baudText.toInt(&baudOk);
    if (isAutoBaudText(ui->defaultBaudCombo, baudText)) {
        serial.autoBaud = true;   // baudRate stays the starting rate the bar opens with
    } else if (baudOk && SerialSettings::isValidBaudRate(baud)) {
        serial.autoBaud = false;
        serial.baudRate = baud;
    } else {
        qCWarning(lcUi) << "Ignoring invalid default baud rate" << ui->defaultBaudCombo->currentText();
        selectDefaultBaud(ui->defaultBaudCombo, serial);
    }
    serial.dataBits = ui->defaultDataBitsCombo->currentData().value<QSerialPort::DataBits>();
    serial.parity = ui->defaultParityCombo->currentData().value<QSerialPort::Parity>();
    serial.stopBits = ui->defaultStopBitsCombo->currentData().value<QSerialPort::StopBits>();
    serial.flowControl = ui->defaultFlowCombo->currentData().value<QSerialPort::FlowControl>();
    serial.dtr = ui->dtrCheck->isChecked();
    serial.rts = ui->rtsCheck->isChecked();
    settings.setDefaultSerialSettings(serial);
    settings.setAutoReconnect(ui->autoReconnectCheck->isChecked());
    settings.setReconnectIntervalMs(ui->reconnectIntervalSpin->value());
    const bool showSimulated = ui->showSimulatedPortsCheck->isChecked();
    if (showSimulated != settings.showSimulatedPorts()) {
        settings.setShowSimulatedPorts(showSimulated);
        SerialPortEnumerator::instance().refresh();   // add / remove the SIM: entries right away
    }
    const QList<qint32> candidates = autoBaudCandidatesFromEdit();
    settings.setAutoBaudCandidates(candidates);   // empty -> the default list
    setAutoBaudCandidatesText(settings.autoBaudCandidates());   // show what was actually stored
    settings.setAutoBaudSampleMs(ui->autoBaudSampleSpin->value());
    settings.setAutoBaudWatchdog(ui->autoBaudWatchdogCheck->isChecked());

    // Keyboard
    saveShortcuts();

    // SSH
    settings.setSshKnownHostsFile(QDir::fromNativeSeparators(ui->sshKnownHostsEdit->text().trimmed()));
    settings.setSshDefaultIdentityFile(QDir::fromNativeSeparators(ui->sshIdentityEdit->text().trimmed()));
    QString terminalType = ui->sshTerminalTypeEdit->text().trimmed();
    if (terminalType.isEmpty()) {
        terminalType = kDefaultSshTerminalType;
        ui->sshTerminalTypeEdit->setText(terminalType);
    }
    settings.setSshDefaultTerminalType(terminalType);
    settings.setSshDefaultKeepAliveSeconds(ui->sshKeepAliveSpin->value());

    // Logging
    QString logDir = ui->logDirEdit->text().trimmed();
    if (logDir.isEmpty()) {
        logDir = QDir::toNativeSeparators(AppSettings::defaultLogDirectory());
        ui->logDirEdit->setText(logDir);
    }
    settings.setLogDirectory(QDir::fromNativeSeparators(logDir));
    settings.setAutoLog(ui->autoLogCheck->isChecked());
    settings.setLogFormat(ui->logFormatCombo->currentData().toString());
    settings.setLogIncludeTx(ui->logIncludeTxCheck->isChecked());

    // General
    settings.setConfirmCloseWhenConnected(ui->confirmCloseCheck->isChecked());
    settings.setRestoreLastPorts(ui->restoreSessionsCheck->isChecked());

    settings.sync();
    qCInfo(lcApp) << "Preferences saved";
}

void PreferencesDialog::onFontButton()
{
    bool ok = false;
    const QFont chosen = QFontDialog::getFont(&ok, m_font, this, tr("Terminal Font"), QFontDialog::MonospacedFonts);
    if (!ok) {
        return;
    }
    m_font = chosen;
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    updateFontPreview();
}

void PreferencesDialog::onBrowseLogDir()
{
    QString start = ui->logDirEdit->text().trimmed();
    if (start.isEmpty() || !QDir(start).exists()) {
        start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Log Directory"), start,
                                                          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        ui->logDirEdit->setText(QDir::toNativeSeparators(dir));
    }
}

void PreferencesDialog::onBrowseKnownHosts()
{
    const QString start = sshBrowseStart(ui->sshKnownHostsEdit->text());
    const QString file = QFileDialog::getOpenFileName(this, tr("Select known_hosts File"), start,
                                                      tr("known_hosts (known_hosts*);;All files (*)"));
    if (!file.isEmpty()) {
        ui->sshKnownHostsEdit->setText(QDir::toNativeSeparators(file));
    }
}

void PreferencesDialog::onBrowseIdentityFile()
{
    const QString start = sshBrowseStart(ui->sshIdentityEdit->text());
    const QString file =
        QFileDialog::getOpenFileName(this, tr("Select Private Key File"), start, tr("All files (*)"));
    if (!file.isEmpty()) {
        ui->sshIdentityEdit->setText(QDir::toNativeSeparators(file));
    }
}

void PreferencesDialog::onRestoreDefaults()
{
    switch (ui->tabWidget->currentIndex()) {
    case PageTerminal:
        m_font = AppSettings::defaultTerminalFont();
        updateFontPreview();
        selectByData(ui->themeCombo, QStringLiteral("dark"));
        ui->scrollbackSpin->setValue(10000);
        ui->cursorBlinkCheck->setChecked(true);
        ui->bellCheck->setChecked(true);
        ui->implicitCrCheck->setChecked(true);
        ui->pauseWhileSelectingCheck->setChecked(true);
        ui->rightClickPastesCheck->setChecked(true);
        break;
    case PageInput:
        selectByData(ui->enterSendsCombo, static_cast<int>(LineEnding::Mode::CR));
        ui->backspaceDeleteCheck->setChecked(true);
        ui->localEchoCheck->setChecked(false);
        selectByData(ui->encodingCombo, QStringLiteral("UTF-8"));
        break;
    case PageConnection: {
        const SerialSettings defaults;   // 115200 8N1, no flow, DTR/RTS asserted, a fixed rate
        selectDefaultBaud(ui->defaultBaudCombo, defaults);
        selectByData(ui->defaultDataBitsCombo, QVariant::fromValue(defaults.dataBits));
        selectByData(ui->defaultParityCombo, QVariant::fromValue(defaults.parity));
        selectByData(ui->defaultStopBitsCombo, QVariant::fromValue(defaults.stopBits));
        selectByData(ui->defaultFlowCombo, QVariant::fromValue(defaults.flowControl));
        ui->dtrCheck->setChecked(defaults.dtr);
        ui->rtsCheck->setChecked(defaults.rts);
        ui->rtsCheck->setEnabled(true);
        ui->autoReconnectCheck->setChecked(true);
        ui->reconnectIntervalSpin->setValue(1000);
        ui->reconnectIntervalSpin->setEnabled(true);
        ui->showSimulatedPortsCheck->setChecked(true);
        setAutoBaudCandidatesText(AppSettings::defaultAutoBaudCandidates());
        ui->autoBaudSampleSpin->setValue(kDefaultAutoBaudSampleMs);
        ui->autoBaudWatchdogCheck->setChecked(true);
        break;
    }
    case PageSsh:
        ui->sshKnownHostsEdit->clear();
        ui->sshIdentityEdit->clear();
        ui->sshTerminalTypeEdit->setText(kDefaultSshTerminalType);
        ui->sshKeepAliveSpin->setValue(kDefaultSshKeepAlive);
        break;
    case PageKeyboard:
        restoreShortcutDefaults();
        break;
    case PageLogging:
        ui->logDirEdit->setText(QDir::toNativeSeparators(AppSettings::defaultLogDirectory()));
        ui->autoLogCheck->setChecked(false);
        selectByData(ui->logFormatCombo, SessionLogger::formatToString(SessionLogger::Format::Text));
        ui->logIncludeTxCheck->setChecked(true);
        break;
    case PageGeneral:
        ui->confirmCloseCheck->setChecked(true);
        ui->restoreSessionsCheck->setChecked(true);
        break;
    default:
        break;
    }
    qCDebug(lcUi) << "Preferences page" << ui->tabWidget->currentIndex() << "reset to defaults (not yet applied)";
}

void PreferencesDialog::onButtonClicked(QAbstractButton* button)
{
    switch (ui->buttonBox->buttonRole(button)) {
    case QDialogButtonBox::AcceptRole:
        saveToSettings();
        emit applied();
        accept();
        break;
    case QDialogButtonBox::ApplyRole:
        saveToSettings();
        emit applied();
        break;
    case QDialogButtonBox::ResetRole:
        onRestoreDefaults();
        break;
    case QDialogButtonBox::RejectRole:
        reject();
        break;
    default:
        break;
    }
}

void PreferencesDialog::updateFontPreview()
{
    ui->fontPreviewLabel->setFont(m_font);
    ui->fontPreviewLabel->setText(QStringLiteral("%1 %2").arg(m_font.family()).arg(m_font.pointSize()));
}

// ---------------------------------------------------------------------------------------
// Connection page: automatic baud-rate detection
// ---------------------------------------------------------------------------------------

QList<qint32> PreferencesDialog::autoBaudCandidatesFromEdit() const
{
    QList<qint32> result;
    const QStringList parts =
        ui->autoBaudCandidatesEdit->text().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const qint32 rate = part.toInt(&ok);
        if (ok && SerialSettings::isValidBaudRate(rate) && !result.contains(rate)) {
            result.append(rate);
        } else if (!ok || !SerialSettings::isValidBaudRate(rate)) {
            qCWarning(lcUi) << "Ignoring invalid auto-baud candidate" << part;
        }
    }
    return result;
}

void PreferencesDialog::setAutoBaudCandidatesText(const QList<qint32>& candidates)
{
    ui->autoBaudCandidatesEdit->setText(joinBaudRates(candidates));
}

// ---------------------------------------------------------------------------------------
// Keyboard page
// ---------------------------------------------------------------------------------------

int PreferencesDialog::currentShortcutRow() const
{
    const QModelIndexList rows = ui->shortcutTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return -1;
    }
    const int row = rows.first().row();
    return (row >= 0 && row < m_shortcutValues.size()) ? row : -1;
}

void PreferencesDialog::loadShortcutRows()
{
    const AppSettings& settings = AppSettings::instance();
    QTableWidget* table = ui->shortcutTable;
    {
        const QSignalBlocker blocker(table->selectionModel());
        table->clearSelection();
        table->setRowCount(0);
        table->setRowCount(static_cast<int>(m_shortcutEntries.size()));
    }
    m_shortcutValues.clear();
    m_shortcutValues.reserve(m_shortcutEntries.size());
    for (int row = 0; row < m_shortcutEntries.size(); ++row) {
        const ShortcutEntry& entry = m_shortcutEntries.at(row);
        auto* actionItem = new QTableWidgetItem(entry.title);
        actionItem->setData(Qt::UserRole, entry.objectName);
        actionItem->setToolTip(entry.objectName);
        auto* shortcutItem = new QTableWidgetItem;
        auto* defaultItem = new QTableWidgetItem(sequenceText(entry.defaultSequence));
        for (QTableWidgetItem* item : {actionItem, shortcutItem, defaultItem}) {
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        }
        table->setItem(row, ColumnAction, actionItem);
        table->setItem(row, ColumnShortcut, shortcutItem);
        table->setItem(row, ColumnDefault, defaultItem);
        m_shortcutValues.append(settings.shortcut(entry.objectName, entry.defaultSequence));
        shortcutItem->setText(sequenceText(m_shortcutValues.last()));
    }
    table->resizeColumnToContents(ColumnShortcut);
    table->resizeColumnToContents(ColumnDefault);
    updateShortcutConflicts();
    onShortcutRowChanged();   // no selection: the editor and buttons are disabled
}

void PreferencesDialog::onShortcutRowChanged()
{
    const int row = currentShortcutRow();
    const bool hasRow = row >= 0;
    ui->shortcutEdit->setEnabled(hasRow);
    ui->shortcutClearButton->setEnabled(hasRow);
    ui->shortcutRestoreButton->setEnabled(hasRow);
    const QSignalBlocker blocker(ui->shortcutEdit);
    ui->shortcutEdit->setKeySequence(hasRow ? m_shortcutValues.at(row) : QKeySequence());
    if (!m_shortcutNotice.isEmpty()) {
        m_shortcutNotice.clear();   // the refusal concerned the previous row
        updateShortcutConflicts();
    }
}

void PreferencesDialog::onShortcutEdited(const QKeySequence& sequence)
{
    const int row = currentShortcutRow();
    if (row < 0) {
        return;
    }
    const QKeySequence chord = firstChord(sequence);
    if (chord.isEmpty()) {
        // QKeySequenceEdit empties itself before it records a key press when its text is
        // selected (the first press after the row was selected with the keyboard), and on
        // Backspace / Delete: neither is a request to clear the row - that is the Clear button -
        // so the row keeps its value; the key that follows is judged on its own.
        return;
    }
    if (!isApplicationShortcutChord(chord)) {
        // A key the connected terminal sends to the device (a letter typed into the editor by
        // mistake, Enter, Space, ...): keep the row's value and say why.
        const QSignalBlocker blocker(ui->shortcutEdit);
        ui->shortcutEdit->setKeySequence(m_shortcutValues.at(row));
        m_shortcutNotice = tr("%1 cannot be a shortcut: while a session is connected it is typed into the device. "
                              "Use Ctrl, Alt or Meta with a key, or an F-key.")
                               .arg(chord.toString(QKeySequence::NativeText));
        qCInfo(lcUi) << "shortcut refused for" << m_shortcutEntries.at(row).objectName << ":" << chord.toString();
        updateShortcutConflicts();
        return;
    }
    if (chord != sequence) {
        const QSignalBlocker blocker(ui->shortcutEdit);
        ui->shortcutEdit->setKeySequence(chord);
    }
    setShortcutValue(row, chord);
}

void PreferencesDialog::onShortcutClear()
{
    const int row = currentShortcutRow();
    if (row < 0) {
        return;
    }
    setShortcutValue(row, QKeySequence());
    const QSignalBlocker blocker(ui->shortcutEdit);
    ui->shortcutEdit->clear();
}

void PreferencesDialog::onShortcutRestore()
{
    const int row = currentShortcutRow();
    if (row < 0) {
        return;
    }
    setShortcutValue(row, m_shortcutEntries.at(row).defaultSequence);
    const QSignalBlocker blocker(ui->shortcutEdit);
    ui->shortcutEdit->setKeySequence(m_shortcutValues.at(row));
}

void PreferencesDialog::setShortcutValue(int row, const QKeySequence& value)
{
    if (row < 0 || row >= m_shortcutValues.size()) {
        return;
    }
    m_shortcutValues[row] = value;
    if (QTableWidgetItem* item = ui->shortcutTable->item(row, ColumnShortcut)) {
        item->setText(sequenceText(value));
    }
    ui->shortcutTable->resizeColumnToContents(ColumnShortcut);
    m_shortcutNotice.clear();   // an accepted edit ends the refusal message
    updateShortcutConflicts();
}

void PreferencesDialog::restoreShortcutDefaults()
{
    for (int row = 0; row < m_shortcutValues.size(); ++row) {
        m_shortcutValues[row] = m_shortcutEntries.at(row).defaultSequence;
        if (QTableWidgetItem* item = ui->shortcutTable->item(row, ColumnShortcut)) {
            item->setText(sequenceText(m_shortcutValues.at(row)));
        }
    }
    ui->shortcutTable->resizeColumnToContents(ColumnShortcut);
    updateShortcutConflicts();
    onShortcutRowChanged();   // the editor shows the selected row's (restored) value
}

void PreferencesDialog::updateShortcutConflicts()
{
    QTableWidget* table = ui->shortcutTable;
    const QBrush normal = table->palette().brush(QPalette::Text);
    const QBrush conflict(QColor(0xC0, 0x39, 0x2B));
    QString message;
    m_shortcutConflict = false;
    for (int row = 0; row < m_shortcutValues.size(); ++row) {
        bool clashes = false;
        const QKeySequence& value = m_shortcutValues.at(row);
        if (!value.isEmpty()) {
            for (int other = 0; other < m_shortcutValues.size(); ++other) {
                if (other != row && m_shortcutValues.at(other) == value) {
                    clashes = true;
                    if (message.isEmpty() && other > row) {
                        message = tr("Conflict: \"%1\" and \"%2\" both use %3.")
                                      .arg(m_shortcutEntries.at(row).title, m_shortcutEntries.at(other).title,
                                           value.toString(QKeySequence::NativeText));
                    }
                }
            }
            // The always-active alternates and menu mnemonics: Qt would see an ambiguous
            // shortcut (the key alternates between the two owners) - except on the row of the
            // action that owns the alternate, where MainWindow simply does not add it twice.
            for (const ShortcutEntry& fixed : std::as_const(m_fixedShortcuts)) {
                if (fixed.defaultSequence == value && fixed.objectName != m_shortcutEntries.at(row).objectName) {
                    clashes = true;
                    if (message.isEmpty()) {
                        message = tr("Conflict: \"%1\" uses %2, which is fixed for \"%3\".")
                                      .arg(m_shortcutEntries.at(row).title, value.toString(QKeySequence::NativeText),
                                           fixed.title);
                    }
                }
            }
        }
        m_shortcutConflict = m_shortcutConflict || clashes;
        for (int column = 0; column < ShortcutColumnCount; ++column) {
            if (QTableWidgetItem* item = table->item(row, column)) {
                item->setForeground(clashes ? conflict : normal);
            }
        }
    }
    if (!m_shortcutConflict && !m_shortcutNotice.isEmpty()) {
        message = m_shortcutNotice;   // why the last edit was refused (no conflict to report)
    }
    ui->shortcutConflictLabel->setText(message);
    ui->shortcutConflictLabel->setVisible(!message.isEmpty());
    if (QPushButton* ok = ui->buttonBox->button(QDialogButtonBox::Ok)) {
        ok->setEnabled(!m_shortcutConflict);
    }
    if (QPushButton* apply = ui->buttonBox->button(QDialogButtonBox::Apply)) {
        apply->setEnabled(!m_shortcutConflict);
    }
}

void PreferencesDialog::saveShortcuts()
{
    if (m_shortcutConflict) {
        qCWarning(lcUi) << "shortcuts not saved: a conflict is unresolved";
        return;
    }
    AppSettings& settings = AppSettings::instance();
    int changed = 0;
    for (int row = 0; row < m_shortcutValues.size(); ++row) {
        const ShortcutEntry& entry = m_shortcutEntries.at(row);
        const QKeySequence value = m_shortcutValues.at(row);
        if (value == settings.shortcut(entry.objectName, entry.defaultSequence)) {
            continue;   // unchanged (stored value or the default in effect)
        }
        ++changed;
        if (value == entry.defaultSequence) {
            settings.clearShortcut(entry.objectName);
        } else {
            settings.setShortcut(entry.objectName, value);   // an empty value = no shortcut
        }
    }
    if (changed > 0) {
        qCInfo(lcUi) << changed << "shortcut(s) changed";
    }
}
