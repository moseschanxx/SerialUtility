#include "dialogs/PreferencesDialog.h"
#include "ui_PreferencesDialog.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontDialog>
#include <QIntValidator>
#include <QPushButton>
#include <QSerialPort>
#include <QStandardPaths>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/LineEnding.h"
#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"
#include "core/SessionLogger.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalTheme.h"

namespace {

// Tab indices as laid out in PreferencesDialog.ui.
enum Page { PageTerminal = 0, PageInput, PageConnection, PageLogging, PageGeneral };

QFont defaultTerminalFont()
{
#if defined(Q_OS_WIN)
    QFont font(QStringLiteral("Consolas"), 10);
#else
    QFont font(QStringLiteral("Monospace"), 10);
#endif
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QString defaultLogDirectory()
{
    return QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                                    + QStringLiteral("/BuildAI/SerialLogs"));
}

/// Select the item whose user data equals `value`; falls back to the first item.
void selectByData(QComboBox* combo, const QVariant& value)
{
    const int index = combo->findData(value);
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

} // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::PreferencesDialog)
    , m_font(defaultTerminalFont())
{
    ui->setupUi(this);
    setWindowTitle(tr("Preferences"));

    setupPages();

    connect(ui->fontButton, &QPushButton::clicked, this, &PreferencesDialog::onFontButton);
    connect(ui->logDirBrowseButton, &QPushButton::clicked, this, &PreferencesDialog::onBrowseLogDir);
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, &PreferencesDialog::onButtonClicked);
    connect(ui->autoReconnectCheck, &QCheckBox::toggled, ui->reconnectIntervalSpin, &QWidget::setEnabled);
    connect(ui->defaultFlowCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto flow = ui->defaultFlowCombo->currentData().value<QSerialPort::FlowControl>();
        ui->rtsCheck->setEnabled(flow != QSerialPort::HardwareControl);
    });

    loadFromSettings();
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
    ui->defaultBaudCombo->setValidator(new QIntValidator(1, 100000000, ui->defaultBaudCombo));
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

    // ---- Logging --------------------------------------------------------------------
    ui->logFormatCombo->clear();
    ui->logFormatCombo->addItem(tr("Raw bytes (replayable capture)"),
                                SessionLogger::formatToString(SessionLogger::Format::Raw));
    ui->logFormatCombo->addItem(tr("Timestamped text"), SessionLogger::formatToString(SessionLogger::Format::Text));
    ui->logFormatCombo->addItem(tr("Hex dump"), SessionLogger::formatToString(SessionLogger::Format::HexDump));
    ui->logDirEdit->setPlaceholderText(defaultLogDirectory());

    ui->tabWidget->setCurrentIndex(PageTerminal);
    updateFontPreview();
}

void PreferencesDialog::loadFromSettings()
{
    const AppSettings& settings = AppSettings::instance();

    // Terminal
    m_font = settings.terminalFont();
    updateFontPreview();
    selectByData(ui->themeCombo, settings.themeName());
    ui->scrollbackSpin->setValue(settings.scrollbackLines());
    ui->cursorBlinkCheck->setChecked(settings.cursorBlink());
    ui->bellCheck->setChecked(settings.bellEnabled());
    ui->implicitCrCheck->setChecked(settings.implicitCr());

    // Input
    selectByData(ui->enterSendsCombo, static_cast<int>(settings.enterSends()));
    ui->backspaceDeleteCheck->setChecked(settings.backspaceSendsDelete());
    ui->localEchoCheck->setChecked(settings.localEcho());
    selectByData(ui->encodingCombo, settings.encoding());

    // Connection
    const SerialSettings serial = settings.defaultSerialSettings();
    selectBaud(ui->defaultBaudCombo, serial.baudRate);
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

    // Input
    settings.setEnterSends(static_cast<LineEnding::Mode>(ui->enterSendsCombo->currentData().toInt()));
    settings.setBackspaceSendsDelete(ui->backspaceDeleteCheck->isChecked());
    settings.setLocalEcho(ui->localEchoCheck->isChecked());
    settings.setEncoding(ui->encodingCombo->currentText());

    // Connection
    SerialSettings serial = settings.defaultSerialSettings();
    bool baudOk = false;
    const qint32 baud = ui->defaultBaudCombo->currentText().trimmed().toInt(&baudOk);
    if (baudOk && baud > 0) {
        serial.baudRate = baud;
    } else {
        qCWarning(lcUi) << "Ignoring invalid default baud rate" << ui->defaultBaudCombo->currentText();
        selectBaud(ui->defaultBaudCombo, serial.baudRate);
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

    // Logging
    QString logDir = ui->logDirEdit->text().trimmed();
    if (logDir.isEmpty()) {
        logDir = defaultLogDirectory();
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

void PreferencesDialog::onRestoreDefaults()
{
    switch (ui->tabWidget->currentIndex()) {
    case PageTerminal:
        m_font = defaultTerminalFont();
        updateFontPreview();
        selectByData(ui->themeCombo, QStringLiteral("dark"));
        ui->scrollbackSpin->setValue(10000);
        ui->cursorBlinkCheck->setChecked(true);
        ui->bellCheck->setChecked(true);
        ui->implicitCrCheck->setChecked(true);
        break;
    case PageInput:
        selectByData(ui->enterSendsCombo, static_cast<int>(LineEnding::Mode::CR));
        ui->backspaceDeleteCheck->setChecked(true);
        ui->localEchoCheck->setChecked(false);
        selectByData(ui->encodingCombo, QStringLiteral("UTF-8"));
        break;
    case PageConnection: {
        const SerialSettings defaults;   // 115200 8N1, no flow, DTR/RTS asserted
        selectBaud(ui->defaultBaudCombo, defaults.baudRate);
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
        break;
    }
    case PageLogging:
        ui->logDirEdit->setText(defaultLogDirectory());
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
