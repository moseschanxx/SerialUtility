// GUI test suite for the dialogs: PreferencesDialog (every control <-> AppSettings, Restore
// Defaults per page), QuickCommandModel / QuickCommandsDialog (editing, persistence, import /
// export through QuickCommandStore), AboutDialog and VersionDialog. Runs offscreen
// (QT_QPA_PLATFORM=offscreen); QSettings and every file live in a temporary directory or the
// QStandardPaths test-mode data directory, so nothing touches the user's real configuration.
#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QIntValidator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSerialPort>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTranslator>

#include "Version.h"
#include "app/AppSettings.h"
#include "core/LineEnding.h"
#include "core/QuickCommand.h"
#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"
#include "core/SessionLogger.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/PreferencesDialog.h"
#include "dialogs/QuickCommandsDialog.h"
#include "dialogs/SendFileDialog.h"
#include "dialogs/VersionDialog.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalTheme.h"

// =======================================================================================
// Helpers
// =======================================================================================

/// Answers modal dialogs from inside their exec() loop: QMessageBox questions get Yes / No,
/// everything else is rejected. Counts what it dismissed so a test can assert that a dialog
/// did (or did not) appear.
class ModalDismisser : public QObject
{
    Q_OBJECT
public:
    enum class Answer { Reject, Yes, No };

    explicit ModalDismisser(Answer answer, QObject* parent = nullptr)
        : QObject(parent)
        , m_answer(answer)
    {
        m_timer.setInterval(25);
        connect(&m_timer, &QTimer::timeout, this, &ModalDismisser::poll);
        m_timer.start();
    }

    void stop() { m_timer.stop(); }
    int count() const { return m_count; }

private:
    void poll()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || !modal->isVisible()) {
            return;
        }
        ++m_count;
        if (auto* box = qobject_cast<QMessageBox*>(modal); box && m_answer != Answer::Reject) {
            QAbstractButton* button = box->button(m_answer == Answer::Yes ? QMessageBox::Yes : QMessageBox::No);
            if (button) {
                button->click();
                return;
            }
        }
        if (auto* dialog = qobject_cast<QDialog*>(modal)) {
            dialog->reject();
        } else {
            modal->close();
        }
    }

    Answer m_answer;
    QTimer m_timer;
    int m_count = 0;
};

namespace {

// Tab indices of PreferencesDialog.ui.
enum Page { PageTerminal = 0, PageInput, PageConnection, PageLogging, PageGeneral };

template <typename T>
T* child(const QObject* parent, const char* objectName)
{
    return parent->findChild<T*>(QLatin1String(objectName));
}

/// Show a top-level widget offscreen and wait until it is exposed.
bool expose(QWidget* widget)
{
    widget->show();
    return QTest::qWaitForWindowExposed(widget);
}

/// Select the combo item whose user data equals `value`; false when absent.
bool selectData(QComboBox* combo, const QVariant& value)
{
    const int index = combo->findData(value);
    if (index < 0) {
        return false;
    }
    combo->setCurrentIndex(index);
    return true;
}

QString defaultFontFamily()
{
    return AppSettings::defaultTerminalFont().family();
}

QString documentsLogDirectory()
{
    return AppSettings::defaultLogDirectory();
}

/// Every control of PreferencesDialog.ui, looked up by objectName.
struct PrefControls
{
    QLabel* fontPreview = nullptr;
    QPushButton* fontButton = nullptr;
    QComboBox* theme = nullptr;
    QSpinBox* scrollback = nullptr;
    QCheckBox* cursorBlink = nullptr;
    QCheckBox* bell = nullptr;
    QCheckBox* implicitCr = nullptr;
    QComboBox* enterSends = nullptr;
    QCheckBox* backspaceDelete = nullptr;
    QCheckBox* localEcho = nullptr;
    QComboBox* encoding = nullptr;
    QComboBox* baud = nullptr;
    QComboBox* dataBits = nullptr;
    QComboBox* parity = nullptr;
    QComboBox* stopBits = nullptr;
    QComboBox* flow = nullptr;
    QCheckBox* dtr = nullptr;
    QCheckBox* rts = nullptr;
    QCheckBox* autoReconnect = nullptr;
    QSpinBox* reconnectInterval = nullptr;
    QCheckBox* showSimulated = nullptr;
    QLineEdit* logDir = nullptr;
    QPushButton* logDirBrowse = nullptr;
    QCheckBox* autoLog = nullptr;
    QComboBox* logFormat = nullptr;
    QCheckBox* logIncludeTx = nullptr;
    QCheckBox* confirmClose = nullptr;
    QCheckBox* restoreSessions = nullptr;
    QTabWidget* tabs = nullptr;
    QDialogButtonBox* buttons = nullptr;

    bool complete() const
    {
        return fontPreview && fontButton && theme && scrollback && cursorBlink && bell && implicitCr && enterSends &&
               backspaceDelete && localEcho && encoding && baud && dataBits && parity && stopBits && flow && dtr &&
               rts && autoReconnect && reconnectInterval && showSimulated && logDir && logDirBrowse && autoLog &&
               logFormat && logIncludeTx && confirmClose && restoreSessions && tabs && buttons;
    }
};

PrefControls controlsOf(const PreferencesDialog& dialog)
{
    PrefControls c;
    c.fontPreview = child<QLabel>(&dialog, "fontPreviewLabel");
    c.fontButton = child<QPushButton>(&dialog, "fontButton");
    c.theme = child<QComboBox>(&dialog, "themeCombo");
    c.scrollback = child<QSpinBox>(&dialog, "scrollbackSpin");
    c.cursorBlink = child<QCheckBox>(&dialog, "cursorBlinkCheck");
    c.bell = child<QCheckBox>(&dialog, "bellCheck");
    c.implicitCr = child<QCheckBox>(&dialog, "implicitCrCheck");
    c.enterSends = child<QComboBox>(&dialog, "enterSendsCombo");
    c.backspaceDelete = child<QCheckBox>(&dialog, "backspaceDeleteCheck");
    c.localEcho = child<QCheckBox>(&dialog, "localEchoCheck");
    c.encoding = child<QComboBox>(&dialog, "encodingCombo");
    c.baud = child<QComboBox>(&dialog, "defaultBaudCombo");
    c.dataBits = child<QComboBox>(&dialog, "defaultDataBitsCombo");
    c.parity = child<QComboBox>(&dialog, "defaultParityCombo");
    c.stopBits = child<QComboBox>(&dialog, "defaultStopBitsCombo");
    c.flow = child<QComboBox>(&dialog, "defaultFlowCombo");
    c.dtr = child<QCheckBox>(&dialog, "dtrCheck");
    c.rts = child<QCheckBox>(&dialog, "rtsCheck");
    c.autoReconnect = child<QCheckBox>(&dialog, "autoReconnectCheck");
    c.reconnectInterval = child<QSpinBox>(&dialog, "reconnectIntervalSpin");
    c.showSimulated = child<QCheckBox>(&dialog, "showSimulatedPortsCheck");
    c.logDir = child<QLineEdit>(&dialog, "logDirEdit");
    c.logDirBrowse = child<QPushButton>(&dialog, "logDirBrowseButton");
    c.autoLog = child<QCheckBox>(&dialog, "autoLogCheck");
    c.logFormat = child<QComboBox>(&dialog, "logFormatCombo");
    c.logIncludeTx = child<QCheckBox>(&dialog, "logIncludeTxCheck");
    c.confirmClose = child<QCheckBox>(&dialog, "confirmCloseCheck");
    c.restoreSessions = child<QCheckBox>(&dialog, "restoreSessionsCheck");
    c.tabs = child<QTabWidget>(&dialog, "tabWidget");
    c.buttons = child<QDialogButtonBox>(&dialog, "buttonBox");
    return c;
}

QuickCommand makeCommand(const QString& name, const QString& command, const QString& group,
                         LineEnding::Mode ending = LineEnding::Mode::CR)
{
    QuickCommand qc;
    qc.name = name;
    qc.command = command;
    qc.group = group;
    qc.lineEnding = ending;
    return qc;
}

QString cellText(const QAbstractItemModel* model, int row, int column)
{
    return model->index(row, column).data(Qt::DisplayRole).toString();
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

} // namespace

// =======================================================================================
// Test class
// =======================================================================================

class Tst_dialogs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- PreferencesDialog ----------------------------------------------------------
    void preferencesLoadsFromSettings();
    void preferencesApplyWritesEveryControl();
    void preferencesOkAcceptsAndWrites();
    void preferencesCancelDiscards();
    void preferencesRestoreDefaultsTerminal();
    void preferencesRestoreDefaultsInput();
    void preferencesRestoreDefaultsConnection();
    void preferencesRestoreDefaultsLogging();
    void preferencesRestoreDefaultsGeneral();
    void preferencesShowSimulatedPortsRoundTrip();
    void preferencesDependentControls();
    void preferencesEmptyLogDirFallsBack();
    void preferencesInvalidBaudIgnored();
    void preferencesKeepsUnlistedEncodingAndTheme();

    // ---- QuickCommandModel ----------------------------------------------------------
    void modelRowsAndColumns();
    void modelDataEveryColumn();
    void modelSetDataEveryColumn();
    void modelFlagsAndCheckStates();
    void modelInsertRemove();
    void modelMove();
    void modelHeaderData();

    // ---- QuickCommandsDialog --------------------------------------------------------
    void dialogShowsStoreCommands();
    void dialogAddButton();
    void dialogRemoveButton();
    void dialogMoveButtons();
    void dialogButtonEnableState();
    void dialogSetCurrentRow();
    void dialogAcceptWritesStoreAndSavesJson();
    void dialogCancelKeepsStore();
    void dialogRestoreDefaultsConfirmed();
    void dialogRestoreDefaultsDeclined();

    // ---- QuickCommandStore import / export ------------------------------------------
    void storeExportImportRoundTrip();
    void storeImportErrors();

    // ---- SendFileDialog -------------------------------------------------------------
    void sendFileBackpressure();
    void sendFileRetranslates();

    // ---- AboutDialog / VersionDialog ------------------------------------------------
    void helpDialogsRetranslate();
    void aboutDialogContent();
    void aboutDialogLogoResource();
    void aboutDialogCloseButton();
    void versionDialogContent();
    void versionDialogCopyButton();
    void versionDialogCloseButton();

private:
    void setNonDefaultSettings();
    QString tempPath(const QString& name) const;

    QTemporaryDir m_tempDir;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_dialogs::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-dialogs"));
    QVERIFY(m_tempDir.isValid());

    // AppSettings uses the default QSettings() constructor: keep it out of the registry and
    // inside the temporary directory, and make sure it picked up the test names above.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("BuildAI-Test")));
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-dialogs")));

    AppSettings::instance().setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    QVERIFY(settings.contains(QStringLiteral("logging/directory")));
    QVERIFY(AppSettings::dataDirectory().contains(QStringLiteral("SerialUtilityTest-dialogs")));
}

void Tst_dialogs::init()
{
    QSettings().clear();
    AppSettings& app = AppSettings::instance();
    app.setLanguage(QStringLiteral("en_US"));
    app.setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    app.setShowSimulatedPorts(true);
    QApplication::clipboard()->clear();
}

/// Distinct, non-default values for every preference (the opposite of AppSettings.h defaults).
void Tst_dialogs::setNonDefaultSettings()
{
    AppSettings& s = AppSettings::instance();
    s.setTerminalFont(QFont(QStringLiteral("Courier New"), 14));
    s.setThemeName(TerminalTheme::names().at(1));
    s.setScrollbackLines(5000);
    s.setCursorBlink(false);
    s.setBellEnabled(false);
    s.setImplicitCr(false);
    s.setEnterSends(LineEnding::Mode::LF);
    s.setBackspaceSendsDelete(false);
    s.setLocalEcho(true);
    s.setEncoding(QStringLiteral("ISO-8859-1"));
    SerialSettings serial;
    serial.baudRate = 1500000;
    serial.dataBits = QSerialPort::Data7;
    serial.parity = QSerialPort::EvenParity;
    serial.stopBits = QSerialPort::TwoStop;
    serial.flowControl = QSerialPort::SoftwareControl;
    serial.dtr = false;
    serial.rts = false;
    s.setDefaultSerialSettings(serial);
    s.setAutoReconnect(false);
    s.setReconnectIntervalMs(2500);
    s.setShowSimulatedPorts(false);
    s.setLogDirectory(tempPath(QStringLiteral("pref-logs")));
    s.setAutoLog(true);
    s.setLogFormat(QStringLiteral("hex"));
    s.setLogIncludeTx(false);
    s.setConfirmCloseWhenConnected(false);
    s.setRestoreLastPorts(false);
}

QString Tst_dialogs::tempPath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

// =======================================================================================
// PreferencesDialog
// =======================================================================================

void Tst_dialogs::preferencesLoadsFromSettings()
{
    QVERIFY(TerminalTheme::names().size() >= 2);
    setNonDefaultSettings();
    const QString theme = TerminalTheme::names().at(1);

    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Preferences"));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QCOMPARE(c.tabs->count(), 5);
    QCOMPARE(c.tabs->currentIndex(), PageTerminal);

    // Terminal
    QCOMPARE(c.fontPreview->text(), QStringLiteral("Courier New 14"));
    QCOMPARE(c.fontPreview->font().family(), QStringLiteral("Courier New"));
    QCOMPARE(c.theme->currentData().toString(), theme);
    QCOMPARE(c.theme->count(), TerminalTheme::names().size());
    QCOMPARE(c.scrollback->value(), 5000);
    QCOMPARE(c.scrollback->minimum(), 100);
    QCOMPARE(c.scrollback->maximum(), 1000000);
    QVERIFY(!c.cursorBlink->isChecked());
    QVERIFY(!c.bell->isChecked());
    QVERIFY(!c.implicitCr->isChecked());

    // Input
    QCOMPARE(c.enterSends->currentData().toInt(), static_cast<int>(LineEnding::Mode::LF));
    QCOMPARE(c.enterSends->count(), LineEnding::allModes().size());
    QVERIFY(!c.backspaceDelete->isChecked());
    QVERIFY(c.localEcho->isChecked());
    QCOMPARE(c.encoding->currentText(), QStringLiteral("ISO-8859-1"));
    QCOMPARE(c.encoding->count(), AnsiParser::availableEncodings().size());

    // Connection
    QCOMPARE(c.baud->currentText(), QStringLiteral("1500000"));
    QVERIFY(c.baud->isEditable());
    QCOMPARE(c.dataBits->currentData().value<QSerialPort::DataBits>(), QSerialPort::Data7);
    QCOMPARE(c.parity->currentData().value<QSerialPort::Parity>(), QSerialPort::EvenParity);
    QCOMPARE(c.stopBits->currentData().value<QSerialPort::StopBits>(), QSerialPort::TwoStop);
    QCOMPARE(c.flow->currentData().value<QSerialPort::FlowControl>(), QSerialPort::SoftwareControl);
    QVERIFY(!c.dtr->isChecked());
    QVERIFY(!c.rts->isChecked());
    QVERIFY(c.rts->isEnabled());   // RTS is only forced by hardware flow control
    QVERIFY(!c.autoReconnect->isChecked());
    QCOMPARE(c.reconnectInterval->value(), 2500);
    QVERIFY(!c.reconnectInterval->isEnabled());
    QCOMPARE(c.reconnectInterval->minimum(), 200);
    QCOMPARE(c.reconnectInterval->maximum(), 60000);
    QVERIFY(!c.showSimulated->isChecked());

    // Logging
    QCOMPARE(c.logDir->text(), QDir::toNativeSeparators(tempPath(QStringLiteral("pref-logs"))));
    QVERIFY(c.autoLog->isChecked());
    QCOMPARE(c.logFormat->currentData().toString(), QStringLiteral("hex"));
    QCOMPARE(c.logFormat->count(), 3);
    QVERIFY(!c.logIncludeTx->isChecked());

    // General
    QVERIFY(!c.confirmClose->isChecked());
    QVERIFY(!c.restoreSessions->isChecked());

    // Button box contract.
    QVERIFY(c.buttons->button(QDialogButtonBox::Ok));
    QVERIFY(c.buttons->button(QDialogButtonBox::Cancel));
    QVERIFY(c.buttons->button(QDialogButtonBox::Apply));
    QVERIFY(c.buttons->button(QDialogButtonBox::RestoreDefaults));
}

void Tst_dialogs::preferencesApplyWritesEveryControl()
{
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QSignalSpy applied(&dialog, &PreferencesDialog::applied);
    const QString theme = TerminalTheme::names().last();
    QVERIFY(theme != QStringLiteral("dark"));
    const QString logDir = tempPath(QStringLiteral("pref-logs-2"));

    // Terminal
    QVERIFY(selectData(c.theme, theme));
    c.scrollback->setValue(20000);
    c.cursorBlink->setChecked(false);
    c.bell->setChecked(false);
    c.implicitCr->setChecked(false);
    // Input
    QVERIFY(selectData(c.enterSends, static_cast<int>(LineEnding::Mode::CRLF)));
    c.backspaceDelete->setChecked(false);
    c.localEcho->setChecked(true);
    QVERIFY(selectData(c.encoding, QStringLiteral("ISO-8859-1")));
    // Connection
    QVERIFY(c.baud->findText(QStringLiteral("921600")) >= 0);
    c.baud->setCurrentIndex(c.baud->findText(QStringLiteral("921600")));
    QVERIFY(selectData(c.dataBits, QVariant::fromValue(QSerialPort::Data7)));
    QVERIFY(selectData(c.parity, QVariant::fromValue(QSerialPort::OddParity)));
    QVERIFY(selectData(c.stopBits, QVariant::fromValue(QSerialPort::TwoStop)));
    QVERIFY(selectData(c.flow, QVariant::fromValue(QSerialPort::HardwareControl)));
    QVERIFY(!c.rts->isEnabled());   // RTS is driven by the hardware flow control
    c.dtr->setChecked(false);
    c.rts->setChecked(false);
    c.autoReconnect->setChecked(false);
    c.reconnectInterval->setValue(3000);
    c.showSimulated->setChecked(false);
    // Logging
    c.logDir->setText(QDir::toNativeSeparators(logDir));
    c.autoLog->setChecked(true);
    QVERIFY(selectData(c.logFormat, SessionLogger::formatToString(SessionLogger::Format::Raw)));
    c.logIncludeTx->setChecked(false);
    // General
    c.confirmClose->setChecked(false);
    c.restoreSessions->setChecked(false);

    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(applied.count(), 1);
    QVERIFY(dialog.isVisible());   // Apply keeps the dialog open

    const AppSettings& s = AppSettings::instance();
    QCOMPARE(s.themeName(), theme);
    QCOMPARE(s.scrollbackLines(), 20000);
    QVERIFY(!s.cursorBlink());
    QVERIFY(!s.bellEnabled());
    QVERIFY(!s.implicitCr());
    QCOMPARE(static_cast<int>(s.enterSends()), static_cast<int>(LineEnding::Mode::CRLF));
    QVERIFY(!s.backspaceSendsDelete());
    QVERIFY(s.localEcho());
    QCOMPARE(s.encoding(), QStringLiteral("ISO-8859-1"));
    const SerialSettings serial = s.defaultSerialSettings();
    QCOMPARE(serial.baudRate, 921600);
    QCOMPARE(serial.dataBits, QSerialPort::Data7);
    QCOMPARE(serial.parity, QSerialPort::OddParity);
    QCOMPARE(serial.stopBits, QSerialPort::TwoStop);
    QCOMPARE(serial.flowControl, QSerialPort::HardwareControl);
    QVERIFY(!serial.dtr);
    QVERIFY(!serial.rts);
    QVERIFY(!s.autoReconnect());
    QCOMPARE(s.reconnectIntervalMs(), 3000);
    QVERIFY(!s.showSimulatedPorts());
    QCOMPARE(s.logDirectory(), logDir);
    QVERIFY(s.autoLog());
    QCOMPARE(s.logFormat(), QStringLiteral("raw"));
    QVERIFY(!s.logIncludeTx());
    QVERIFY(!s.confirmCloseWhenConnected());
    QVERIFY(!s.restoreLastPorts());
    QVERIFY(QSettings().contains(QStringLiteral("terminal/font")));   // the (unchanged) font is written too
    QCOMPARE(s.terminalFont().family(), defaultFontFamily());

    // A second dialog shows the applied values.
    PreferencesDialog again;
    const PrefControls c2 = controlsOf(again);
    QVERIFY(c2.complete());
    QCOMPARE(c2.theme->currentData().toString(), theme);
    QCOMPARE(c2.scrollback->value(), 20000);
    QCOMPARE(c2.baud->currentText(), QStringLiteral("921600"));
    QCOMPARE(c2.flow->currentData().value<QSerialPort::FlowControl>(), QSerialPort::HardwareControl);
    QVERIFY(!c2.rts->isEnabled());
    QCOMPARE(c2.logDir->text(), QDir::toNativeSeparators(logDir));
    QVERIFY(!c2.showSimulated->isChecked());
}

void Tst_dialogs::preferencesOkAcceptsAndWrites()
{
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QSignalSpy applied(&dialog, &PreferencesDialog::applied);

    c.scrollback->setValue(777);
    c.localEcho->setChecked(true);
    c.buttons->button(QDialogButtonBox::Ok)->click();

    QCOMPARE(applied.count(), 1);
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(!dialog.isVisible());
    QCOMPARE(AppSettings::instance().scrollbackLines(), 777);
    QVERIFY(AppSettings::instance().localEcho());
}

void Tst_dialogs::preferencesCancelDiscards()
{
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QSignalSpy applied(&dialog, &PreferencesDialog::applied);

    c.scrollback->setValue(777);
    c.localEcho->setChecked(true);
    c.confirmClose->setChecked(false);
    c.buttons->button(QDialogButtonBox::Cancel)->click();

    QCOMPARE(applied.count(), 0);
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    QVERIFY(!dialog.isVisible());
    QCOMPARE(AppSettings::instance().scrollbackLines(), 10000);
    QVERIFY(!AppSettings::instance().localEcho());
    QVERIFY(AppSettings::instance().confirmCloseWhenConnected());
}

void Tst_dialogs::preferencesRestoreDefaultsTerminal()
{
    setNonDefaultSettings();
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    c.tabs->setCurrentIndex(PageTerminal);
    c.buttons->button(QDialogButtonBox::RestoreDefaults)->click();

    QCOMPARE(c.fontPreview->text(), defaultFontFamily() + QStringLiteral(" 10"));
    QCOMPARE(c.theme->currentData().toString(), QStringLiteral("dark"));
    QCOMPARE(c.scrollback->value(), 10000);
    QVERIFY(c.cursorBlink->isChecked());
    QVERIFY(c.bell->isChecked());
    QVERIFY(c.implicitCr->isChecked());

    // Other pages are untouched and nothing is written until Apply.
    QCOMPARE(c.enterSends->currentData().toInt(), static_cast<int>(LineEnding::Mode::LF));
    QVERIFY(!c.autoReconnect->isChecked());
    QCOMPARE(AppSettings::instance().scrollbackLines(), 5000);
    QCOMPARE(AppSettings::instance().themeName(), TerminalTheme::names().at(1));

    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().scrollbackLines(), 10000);
    QCOMPARE(AppSettings::instance().themeName(), QStringLiteral("dark"));
    QCOMPARE(AppSettings::instance().terminalFont().family(), defaultFontFamily());
    QCOMPARE(AppSettings::instance().terminalFont().pointSize(), 10);
    // The Input page was not reset.
    QCOMPARE(static_cast<int>(AppSettings::instance().enterSends()), static_cast<int>(LineEnding::Mode::LF));
}

void Tst_dialogs::preferencesRestoreDefaultsInput()
{
    setNonDefaultSettings();
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    c.tabs->setCurrentIndex(PageInput);
    c.buttons->button(QDialogButtonBox::RestoreDefaults)->click();

    QCOMPARE(c.enterSends->currentData().toInt(), static_cast<int>(LineEnding::Mode::CR));
    QVERIFY(c.backspaceDelete->isChecked());
    QVERIFY(!c.localEcho->isChecked());
    QCOMPARE(c.encoding->currentText(), QStringLiteral("UTF-8"));
    QCOMPARE(c.scrollback->value(), 5000);   // Terminal page untouched
    QCOMPARE(static_cast<int>(AppSettings::instance().enterSends()), static_cast<int>(LineEnding::Mode::LF));
}

void Tst_dialogs::preferencesRestoreDefaultsConnection()
{
    setNonDefaultSettings();
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    c.tabs->setCurrentIndex(PageConnection);
    c.buttons->button(QDialogButtonBox::RestoreDefaults)->click();

    QCOMPARE(c.baud->currentText(), QStringLiteral("115200"));
    QCOMPARE(c.dataBits->currentData().value<QSerialPort::DataBits>(), QSerialPort::Data8);
    QCOMPARE(c.parity->currentData().value<QSerialPort::Parity>(), QSerialPort::NoParity);
    QCOMPARE(c.stopBits->currentData().value<QSerialPort::StopBits>(), QSerialPort::OneStop);
    QCOMPARE(c.flow->currentData().value<QSerialPort::FlowControl>(), QSerialPort::NoFlowControl);
    QVERIFY(c.dtr->isChecked());
    QVERIFY(c.rts->isChecked());
    QVERIFY(c.rts->isEnabled());
    QVERIFY(c.autoReconnect->isChecked());
    QCOMPARE(c.reconnectInterval->value(), 1000);
    QVERIFY(c.reconnectInterval->isEnabled());
    QVERIFY(c.showSimulated->isChecked());
    QVERIFY(c.autoLog->isChecked());   // Logging page untouched
    QCOMPARE(AppSettings::instance().defaultSerialSettings().baudRate, 1500000);

    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().defaultSerialSettings(), SerialSettings());
    QVERIFY(AppSettings::instance().autoReconnect());
    QCOMPARE(AppSettings::instance().reconnectIntervalMs(), 1000);
    QVERIFY(AppSettings::instance().showSimulatedPorts());
}

void Tst_dialogs::preferencesRestoreDefaultsLogging()
{
    setNonDefaultSettings();
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    c.tabs->setCurrentIndex(PageLogging);
    c.buttons->button(QDialogButtonBox::RestoreDefaults)->click();

    QCOMPARE(c.logDir->text(), QDir::toNativeSeparators(documentsLogDirectory()));
    QVERIFY(!c.autoLog->isChecked());
    QCOMPARE(c.logFormat->currentData().toString(), SessionLogger::formatToString(SessionLogger::Format::Text));
    QCOMPARE(c.logFormat->currentData().toString(), QStringLiteral("text"));
    QVERIFY(c.logIncludeTx->isChecked());
    QVERIFY(!c.confirmClose->isChecked());   // General page untouched
    QCOMPARE(AppSettings::instance().logFormat(), QStringLiteral("hex"));

    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().logDirectory(), documentsLogDirectory());
    QVERIFY(!AppSettings::instance().autoLog());
    QCOMPARE(AppSettings::instance().logFormat(), QStringLiteral("text"));
    QVERIFY(AppSettings::instance().logIncludeTx());
}

void Tst_dialogs::preferencesRestoreDefaultsGeneral()
{
    setNonDefaultSettings();
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    c.tabs->setCurrentIndex(PageGeneral);
    c.buttons->button(QDialogButtonBox::RestoreDefaults)->click();

    QVERIFY(c.confirmClose->isChecked());
    QVERIFY(c.restoreSessions->isChecked());
    QVERIFY(c.autoLog->isChecked());   // Logging page untouched
    QVERIFY(!AppSettings::instance().confirmCloseWhenConnected());

    c.buttons->button(QDialogButtonBox::Ok)->click();
    QVERIFY(AppSettings::instance().confirmCloseWhenConnected());
    QVERIFY(AppSettings::instance().restoreLastPorts());
    QVERIFY(AppSettings::instance().autoLog());   // still the non-default value
}

void Tst_dialogs::preferencesShowSimulatedPortsRoundTrip()
{
    QVERIFY(AppSettings::instance().showSimulatedPorts());
    SerialPortEnumerator& enumerator = SerialPortEnumerator::instance();
    enumerator.refresh();
    QVERIFY(enumerator.portNames().contains(QStringLiteral("SIM:loopback")));

    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QVERIFY(c.showSimulated->isChecked());

    c.showSimulated->setChecked(false);
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QVERIFY(!AppSettings::instance().showSimulatedPorts());
    // saveToSettings() refreshes the enumerator right away so the SIM: entries disappear.
    const QStringList without = enumerator.portNames();
    for (const QString& name : without) {
        QVERIFY2(!name.startsWith(QStringLiteral("SIM:")), qPrintable(name));
    }

    c.showSimulated->setChecked(true);
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QVERIFY(AppSettings::instance().showSimulatedPorts());
    QVERIFY(enumerator.portNames().contains(QStringLiteral("SIM:loopback")));
    QVERIFY(enumerator.portNames().contains(QStringLiteral("SIM:mcu")));

    PreferencesDialog again;
    QVERIFY(controlsOf(again).showSimulated->isChecked());
}

void Tst_dialogs::preferencesDependentControls()
{
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    QVERIFY(c.autoReconnect->isChecked());
    QVERIFY(c.reconnectInterval->isEnabled());
    c.autoReconnect->setChecked(false);
    QVERIFY(!c.reconnectInterval->isEnabled());
    c.autoReconnect->setChecked(true);
    QVERIFY(c.reconnectInterval->isEnabled());

    QVERIFY(c.rts->isEnabled());
    QVERIFY(selectData(c.flow, QVariant::fromValue(QSerialPort::HardwareControl)));
    QVERIFY(!c.rts->isEnabled());
    QVERIFY(selectData(c.flow, QVariant::fromValue(QSerialPort::SoftwareControl)));
    QVERIFY(c.rts->isEnabled());
    QVERIFY(selectData(c.flow, QVariant::fromValue(QSerialPort::NoFlowControl)));
    QVERIFY(c.rts->isEnabled());

    // The spin boxes clamp to the documented ranges.
    c.scrollback->setValue(1);
    QCOMPARE(c.scrollback->value(), 100);
    c.scrollback->setValue(5000000);
    QCOMPARE(c.scrollback->value(), 1000000);
    c.reconnectInterval->setValue(1);
    QCOMPARE(c.reconnectInterval->value(), 200);
    c.reconnectInterval->setValue(100000000);
    QCOMPARE(c.reconnectInterval->value(), 60000);
}

void Tst_dialogs::preferencesEmptyLogDirFallsBack()
{
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QCOMPARE(c.logDir->text(), QDir::toNativeSeparators(tempPath(QStringLiteral("logs"))));
    QVERIFY(!c.logDir->placeholderText().isEmpty());

    c.logDir->setText(QStringLiteral("   "));
    c.buttons->button(QDialogButtonBox::Apply)->click();

    QCOMPARE(AppSettings::instance().logDirectory(), documentsLogDirectory());
    QCOMPARE(c.logDir->text(), QDir::toNativeSeparators(documentsLogDirectory()));
}

void Tst_dialogs::preferencesInvalidBaudIgnored()
{
    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());
    QCOMPARE(c.baud->currentText(), QStringLiteral("115200"));

    // A custom (non-standard) rate typed into the editable combo is accepted...
    c.baud->setEditText(QStringLiteral("250000"));
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().defaultSerialSettings().baudRate, 250000);

    // ...while an unparsable / zero rate keeps the previous value and re-selects it.
    c.baud->setEditText(QStringLiteral("0"));
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().defaultSerialSettings().baudRate, 250000);
    QCOMPARE(c.baud->currentText(), QStringLiteral("250000"));

    // ...and so does a rate outside SerialSettings::kMinBaudRate..kMaxBaudRate (the range the
    // ConnectionBar accepts), so a default the bar would silently replace can never be stored.
    c.baud->setEditText(QStringLiteral("20000000"));
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().defaultSerialSettings().baudRate, 250000);
    QCOMPARE(c.baud->currentText(), QStringLiteral("250000"));

    c.baud->setEditText(QStringLiteral("20"));
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(AppSettings::instance().defaultSerialSettings().baudRate, 250000);
    QCOMPARE(c.baud->currentText(), QStringLiteral("250000"));

    // The editor's validator shares the same range.
    const auto* validator = qobject_cast<const QIntValidator*>(c.baud->validator());
    QVERIFY(validator);
    QCOMPARE(validator->bottom(), SerialSettings::kMinBaudRate);
    QCOMPARE(validator->top(), SerialSettings::kMaxBaudRate);
}

void Tst_dialogs::preferencesKeepsUnlistedEncodingAndTheme()
{
    AppSettings& s = AppSettings::instance();
    s.setEncoding(QStringLiteral("ISO-8859-15"));   // valid for QStringDecoder with ICU, never in availableEncodings()
    s.setThemeName(QStringLiteral("no-such-theme"));
    s.setScrollbackLines(5000);

    PreferencesDialog dialog;
    QVERIFY(expose(&dialog));
    const PrefControls c = controlsOf(dialog);
    QVERIFY(c.complete());

    // The stored values are appended to the lists and selected instead of falling back to item 0.
    QCOMPARE(c.encoding->currentData().toString(), QStringLiteral("ISO-8859-15"));
    QCOMPARE(c.encoding->count(), AnsiParser::availableEncodings().size() + 1);
    QCOMPARE(c.theme->currentData().toString(), QStringLiteral("no-such-theme"));
    QCOMPARE(c.theme->count(), TerminalTheme::names().size() + 1);

    // Changing an unrelated control and applying leaves the untouched settings untouched.
    c.scrollback->setValue(20000);
    c.buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(s.scrollbackLines(), 20000);
    QCOMPARE(s.encoding(), QStringLiteral("ISO-8859-15"));
    QCOMPARE(s.themeName(), QStringLiteral("no-such-theme"));
}

// =======================================================================================
// QuickCommandModel
// =======================================================================================

void Tst_dialogs::modelRowsAndColumns()
{
    QuickCommandModel model;
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.columnCount(), static_cast<int>(QuickCommandModel::ColumnCount));
    QCOMPARE(model.columnCount(), 8);
    QVERIFY(model.commands().isEmpty());

    const QList<QuickCommand> commands = {
        makeCommand(QStringLiteral("a"), QStringLiteral("cmd a"), QStringLiteral("G")),
        makeCommand(QStringLiteral("b"), QStringLiteral("cmd b"), QString())};
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    model.setCommands(commands);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(model.commands() == commands);

    // Children of a valid index do not exist (flat table).
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    QCOMPARE(model.columnCount(model.index(0, 0)), 0);
    QVERIFY(!model.data(QModelIndex()).isValid());
    QVERIFY(!model.data(model.index(5, 0)).isValid());
}

void Tst_dialogs::modelDataEveryColumn()
{
    QuickCommand text = makeCommand(QStringLiteral("uname"), QStringLiteral("uname -a"), QStringLiteral("Linux"),
                                    LineEnding::Mode::CRLF);
    text.escapes = true;
    text.shortcut = QStringLiteral("Ctrl+1");
    text.tooltip = QStringLiteral("kernel info");
    QuickCommand hex = makeCommand(QStringLiteral("Ctrl+C"), QStringLiteral("03"), QString());
    hex.hex = true;

    QuickCommandModel model;
    model.setCommands({text, hex});
    using C = QuickCommandModel;

    // Name
    QCOMPARE(model.index(0, C::Name).data(Qt::DisplayRole).toString(), QStringLiteral("uname"));
    QCOMPARE(model.index(0, C::Name).data(Qt::EditRole).toString(), QStringLiteral("uname"));
    QCOMPARE(model.index(0, C::Name).data(Qt::ToolTipRole).toString(), QStringLiteral("kernel info"));
    QVERIFY(!model.index(1, C::Name).data(Qt::ToolTipRole).isValid());   // no tooltip set
    // Command
    QCOMPARE(model.index(0, C::Command).data(Qt::DisplayRole).toString(), QStringLiteral("uname -a"));
    QCOMPARE(model.index(0, C::Command).data(Qt::EditRole).toString(), QStringLiteral("uname -a"));
    QVERIFY(model.index(0, C::Command).data(Qt::FontRole).canConvert<QFont>());
    QVERIFY(model.index(0, C::Command).data(Qt::ToolTipRole).toString().contains(QStringLiteral("10")));   // 8 + CRLF
    QCOMPARE(model.index(1, C::Command).data(Qt::DisplayRole).toString(), QStringLiteral("03"));
    QVERIFY(model.index(1, C::Command).data(Qt::ToolTipRole).toString().contains(QStringLiteral("1")));   // one byte
    // Group
    QCOMPARE(model.index(0, C::Group).data(Qt::DisplayRole).toString(), QStringLiteral("Linux"));
    QCOMPARE(model.index(0, C::Group).data(Qt::EditRole).toString(), QStringLiteral("Linux"));
    QCOMPARE(model.index(1, C::Group).data(Qt::DisplayRole).toString(), QStringLiteral("General"));
    QCOMPARE(model.index(1, C::Group).data(Qt::EditRole).toString(), QString());
    // Line ending
    QCOMPARE(model.index(0, C::LineEndingCol).data(Qt::DisplayRole).toString(),
             LineEnding::displayName(LineEnding::Mode::CRLF));
    QCOMPARE(model.index(0, C::LineEndingCol).data(Qt::EditRole).toInt(), static_cast<int>(LineEnding::Mode::CRLF));
    QCOMPARE(model.index(1, C::LineEndingCol).data(Qt::DisplayRole).toString(), QStringLiteral("-"));
    QVERIFY(!model.index(1, C::LineEndingCol).data(Qt::ToolTipRole).toString().isEmpty());
    QVERIFY(!model.index(0, C::LineEndingCol).data(Qt::ToolTipRole).isValid());
    // HEX
    QCOMPARE(model.index(0, C::Hex).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    QCOMPARE(model.index(1, C::Hex).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
    QVERIFY(!model.index(1, C::Hex).data(Qt::DisplayRole).isValid());
    QCOMPARE(model.index(1, C::Hex).data(Qt::TextAlignmentRole).toInt(), static_cast<int>(Qt::AlignCenter));
    QVERIFY(!model.index(1, C::Hex).data(Qt::ToolTipRole).toString().isEmpty());
    // Escapes
    QCOMPARE(model.index(0, C::Escapes).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
    QCOMPARE(model.index(1, C::Escapes).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    QCOMPARE(model.index(0, C::Escapes).data(Qt::TextAlignmentRole).toInt(), static_cast<int>(Qt::AlignCenter));
    // Shortcut
    QCOMPARE(model.index(0, C::Shortcut).data(Qt::DisplayRole).toString(),
             QKeySequence(QStringLiteral("Ctrl+1"), QKeySequence::PortableText).toString(QKeySequence::NativeText));
    QCOMPARE(model.index(0, C::Shortcut).data(Qt::EditRole).toString(), QStringLiteral("Ctrl+1"));
    QCOMPARE(model.index(1, C::Shortcut).data(Qt::DisplayRole).toString(), QString());
    QVERIFY(!model.index(1, C::Shortcut).data(Qt::ToolTipRole).toString().isEmpty());
    QVERIFY(!model.index(0, C::Shortcut).data(Qt::ForegroundRole).isValid());   // Ctrl+1 is terminal-safe
    QVERIFY(!model.index(1, C::Shortcut).data(Qt::ForegroundRole).isValid());   // empty: nothing to flag
    // Shortcuts the connected, focused terminal would swallow before the shortcut map.
    const auto safe = [](const char* text) {
        return QuickCommandModel::isTerminalSafeShortcut(
            QKeySequence(QLatin1String(text), QKeySequence::PortableText));
    };
    QVERIFY(safe("Ctrl+1"));
    QVERIFY(safe("Ctrl+Shift+2"));
    QVERIFY(safe("Meta+R"));
    QVERIFY(!safe("Alt+Shift+R"));
    QVERIFY(!safe("Ctrl+R"));
    QVERIFY(!safe("Ctrl+0"));
    QVERIFY(!safe("F5"));
    QVERIFY(!safe("Ctrl+F5"));
    QVERIFY(!safe("R"));
    QVERIFY(!safe("Shift+R"));
    QVERIFY(!safe("Ctrl+T"));
    QVERIFY(!safe("Ctrl+Tab"));
    QVERIFY(!safe("Ctrl+Ins"));
    QVERIFY(!safe(""));
    QVERIFY(!safe("Ctrl+1, Ctrl+2"));   // multi-stroke sequences are not offered
    // Tooltip
    QCOMPARE(model.index(0, C::Tooltip).data(Qt::DisplayRole).toString(), QStringLiteral("kernel info"));
    QCOMPARE(model.index(0, C::Tooltip).data(Qt::EditRole).toString(), QStringLiteral("kernel info"));
    QCOMPARE(model.index(1, C::Tooltip).data(Qt::DisplayRole).toString(), QString());
}

void Tst_dialogs::modelSetDataEveryColumn()
{
    QuickCommandModel model;
    model.setCommands({makeCommand(QStringLiteral("old"), QStringLiteral("old cmd"), QStringLiteral("Old"))});
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    using C = QuickCommandModel;

    QVERIFY(model.setData(model.index(0, C::Name), QStringLiteral("  new name  ")));
    QCOMPARE(model.commands().first().name, QStringLiteral("new name"));   // trimmed
    QVERIFY(model.setData(model.index(0, C::Command), QStringLiteral("echo hi")));
    QCOMPARE(model.commands().first().command, QStringLiteral("echo hi"));
    QVERIFY(model.setData(model.index(0, C::Group), QStringLiteral(" MCU ")));
    QCOMPARE(model.commands().first().group, QStringLiteral("MCU"));
    QVERIFY(model.setData(model.index(0, C::LineEndingCol), static_cast<int>(LineEnding::Mode::LF)));
    QCOMPARE(static_cast<int>(model.commands().first().lineEnding), static_cast<int>(LineEnding::Mode::LF));
    QVERIFY(model.setData(model.index(0, C::LineEndingCol), QStringLiteral("crlf")));   // settings key form
    QCOMPARE(static_cast<int>(model.commands().first().lineEnding), static_cast<int>(LineEnding::Mode::CRLF));
    QVERIFY(model.setData(model.index(0, C::Escapes), true, Qt::EditRole));
    QVERIFY(model.commands().first().escapes);
    QVERIFY(model.setData(model.index(0, C::Escapes), static_cast<int>(Qt::Unchecked), Qt::CheckStateRole));
    QVERIFY(!model.commands().first().escapes);
    QVERIFY(model.setData(model.index(0, C::Shortcut), QStringLiteral("ctrl+2")));
    QCOMPARE(model.commands().first().shortcut, QStringLiteral("Ctrl+2"));   // normalised portable text
    // A terminal-unsafe shortcut is accepted but flagged (tooltip + warning colour); a safe one is not.
    QVERIFY(model.setData(model.index(0, C::Shortcut), QStringLiteral("Alt+Shift+R")));
    QCOMPARE(model.commands().first().shortcut, QStringLiteral("Alt+Shift+R"));
    QVERIFY(model.index(0, C::Shortcut).data(Qt::ToolTipRole).toString().contains(QStringLiteral("consumed")));
    QVERIFY(model.index(0, C::Shortcut).data(Qt::ForegroundRole).isValid());
    QVERIFY(model.setData(model.index(0, C::Shortcut), QStringLiteral("Ctrl+1")));
    QVERIFY(!model.index(0, C::Shortcut).data(Qt::ToolTipRole).toString().contains(QStringLiteral("consumed")));
    QVERIFY(!model.index(0, C::Shortcut).data(Qt::ForegroundRole).isValid());
    QVERIFY(model.setData(model.index(0, C::Shortcut), QString()));
    QCOMPARE(model.commands().first().shortcut, QString());
    QVERIFY(model.setData(model.index(0, C::Tooltip), QStringLiteral("tip")));
    QCOMPARE(model.commands().first().tooltip, QStringLiteral("tip"));
    QCOMPARE(changed.count(), 12);

    // Checking HEX also refreshes the Command / Line Ending cells (two dataChanged emissions).
    changed.clear();
    QVERIFY(model.setData(model.index(0, C::Hex), static_cast<int>(Qt::Checked), Qt::CheckStateRole));
    QVERIFY(model.commands().first().hex);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(model.index(0, C::LineEndingCol).data(Qt::DisplayRole).toString(), QStringLiteral("-"));

    // Unchanged values succeed without notification; invalid indices / roles fail.
    changed.clear();
    QVERIFY(model.setData(model.index(0, C::Name), QStringLiteral("new name")));
    QCOMPARE(changed.count(), 0);
    QVERIFY(!model.setData(QModelIndex(), QStringLiteral("x")));
    QVERIFY(!model.setData(model.index(3, C::Name), QStringLiteral("x")));
    QVERIFY(model.setData(model.index(0, C::Name), QStringLiteral("ignored"), Qt::DisplayRole));   // wrong role: no-op
    QCOMPARE(model.commands().first().name, QStringLiteral("new name"));
}

void Tst_dialogs::modelFlagsAndCheckStates()
{
    QuickCommandModel model;
    model.setCommands({makeCommand(QStringLiteral("a"), QStringLiteral("a"), QString())});
    using C = QuickCommandModel;

    QCOMPARE(model.flags(QModelIndex()), Qt::NoItemFlags);
    for (int column = 0; column < C::ColumnCount; ++column) {
        const Qt::ItemFlags flags = model.flags(model.index(0, column));
        QVERIFY2(flags.testFlag(Qt::ItemIsEnabled), qPrintable(QString::number(column)));
        QVERIFY2(flags.testFlag(Qt::ItemIsSelectable), qPrintable(QString::number(column)));
        const bool checkbox = (column == C::Hex || column == C::Escapes);
        QCOMPARE(flags.testFlag(Qt::ItemIsUserCheckable), checkbox);
        QCOMPARE(flags.testFlag(Qt::ItemIsEditable), !checkbox);
    }

    // Toggling through the check-state role round-trips.
    QCOMPARE(model.index(0, C::Hex).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    QVERIFY(model.setData(model.index(0, C::Hex), static_cast<int>(Qt::Checked), Qt::CheckStateRole));
    QCOMPARE(model.index(0, C::Hex).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
    QVERIFY(model.setData(model.index(0, C::Hex), static_cast<int>(Qt::Unchecked), Qt::CheckStateRole));
    QCOMPARE(model.index(0, C::Hex).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    QVERIFY(model.setData(model.index(0, C::Escapes), static_cast<int>(Qt::Checked), Qt::CheckStateRole));
    QCOMPARE(model.index(0, C::Escapes).data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
    QVERIFY(!model.index(0, C::Name).data(Qt::CheckStateRole).isValid());
}

void Tst_dialogs::modelInsertRemove()
{
    QuickCommandModel model;
    model.setCommands({makeCommand(QStringLiteral("b"), QStringLiteral("b"), QString())});
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);

    model.insertCommand(0, makeCommand(QStringLiteral("a"), QStringLiteral("a"), QString()));
    model.insertCommand(99, makeCommand(QStringLiteral("z"), QStringLiteral("z"), QString()));   // clamped to the end
    model.insertCommand(-5, makeCommand(QStringLiteral("first"), QStringLiteral("f"), QString()));   // clamped to 0
    QCOMPARE(inserted.count(), 3);
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(cellText(&model, 0, QuickCommandModel::Name), QStringLiteral("first"));
    QCOMPARE(cellText(&model, 1, QuickCommandModel::Name), QStringLiteral("a"));
    QCOMPARE(cellText(&model, 2, QuickCommandModel::Name), QStringLiteral("b"));
    QCOMPARE(cellText(&model, 3, QuickCommandModel::Name), QStringLiteral("z"));

    model.removeCommand(-1);
    model.removeCommand(4);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(model.rowCount(), 4);

    model.removeCommand(0);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(cellText(&model, 0, QuickCommandModel::Name), QStringLiteral("a"));
    model.removeCommand(2);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(cellText(&model, 1, QuickCommandModel::Name), QStringLiteral("b"));
}

void Tst_dialogs::modelMove()
{
    QuickCommandModel model;
    model.setCommands({makeCommand(QStringLiteral("a"), QStringLiteral("a"), QString()),
                       makeCommand(QStringLiteral("b"), QStringLiteral("b"), QString()),
                       makeCommand(QStringLiteral("c"), QStringLiteral("c"), QString())});
    QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);

    QVERIFY(model.moveCommand(0, 2));   // down to the end
    QCOMPARE(cellText(&model, 0, QuickCommandModel::Name), QStringLiteral("b"));
    QCOMPARE(cellText(&model, 1, QuickCommandModel::Name), QStringLiteral("c"));
    QCOMPARE(cellText(&model, 2, QuickCommandModel::Name), QStringLiteral("a"));
    QVERIFY(model.moveCommand(2, 0));   // back up to the top
    QCOMPARE(cellText(&model, 0, QuickCommandModel::Name), QStringLiteral("a"));
    QCOMPARE(cellText(&model, 1, QuickCommandModel::Name), QStringLiteral("b"));
    QVERIFY(model.moveCommand(1, 2));   // adjacent swap downwards
    QCOMPARE(cellText(&model, 1, QuickCommandModel::Name), QStringLiteral("c"));
    QCOMPARE(cellText(&model, 2, QuickCommandModel::Name), QStringLiteral("b"));
    QVERIFY(model.moveCommand(2, 1));   // adjacent swap upwards
    QCOMPARE(cellText(&model, 1, QuickCommandModel::Name), QStringLiteral("b"));
    QCOMPARE(moved.count(), 4);

    QVERIFY(!model.moveCommand(1, 1));
    QVERIFY(!model.moveCommand(-1, 0));
    QVERIFY(!model.moveCommand(0, 3));
    QVERIFY(!model.moveCommand(3, 0));
    QCOMPARE(moved.count(), 4);
}

void Tst_dialogs::modelHeaderData()
{
    QuickCommandModel model;
    const QStringList expected = {QStringLiteral("Name"),    QStringLiteral("Command"),  QStringLiteral("Group"),
                                  QStringLiteral("Line Ending"), QStringLiteral("HEX"),  QStringLiteral("Escapes"),
                                  QStringLiteral("Shortcut"), QStringLiteral("Tooltip")};
    QCOMPARE(expected.size(), static_cast<int>(QuickCommandModel::ColumnCount));
    for (int column = 0; column < QuickCommandModel::ColumnCount; ++column) {
        QCOMPARE(model.headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(), expected.at(column));
        QVERIFY(!model.headerData(column, Qt::Horizontal, Qt::DecorationRole).isValid());
    }
    QVERIFY(!model.headerData(QuickCommandModel::ColumnCount, Qt::Horizontal, Qt::DisplayRole).isValid());
    QCOMPARE(model.headerData(0, Qt::Vertical, Qt::DisplayRole).toInt(), 1);
    QCOMPARE(model.headerData(4, Qt::Vertical, Qt::DisplayRole).toInt(), 5);
    QVERIFY(!model.headerData(0, Qt::Vertical, Qt::ToolTipRole).isValid());
}

// =======================================================================================
// QuickCommandsDialog
// =======================================================================================

void Tst_dialogs::dialogShowsStoreCommands()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("show.json"))));   // missing file -> defaults
    const QList<QuickCommand> defaults = QuickCommandStore::defaults();
    QVERIFY(store.commands() == defaults);

    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Quick Commands"));
    auto* table = child<QTableView>(&dialog, "tableView");
    QVERIFY(table);
    QVERIFY(table->model());
    QCOMPARE(table->model()->rowCount(), defaults.size());
    QCOMPARE(table->model()->columnCount(), static_cast<int>(QuickCommandModel::ColumnCount));
    QCOMPARE(cellText(table->model(), 0, QuickCommandModel::Name), defaults.first().name);
    QCOMPARE(table->currentIndex().row(), 0);   // first row preselected
    for (const char* name :
         {"addButton", "removeButton", "moveUpButton", "moveDownButton", "importButton", "exportButton",
          "restoreDefaultsButton"}) {
        QVERIFY2(child<QPushButton>(&dialog, name) != nullptr, name);
    }
    auto* buttons = child<QDialogButtonBox>(&dialog, "buttonBox");
    QVERIFY(buttons);
    QVERIFY(buttons->button(QDialogButtonBox::Ok));
    QVERIFY(buttons->button(QDialogButtonBox::Cancel));
}

void Tst_dialogs::dialogAddButton()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("add.json"))));
    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");
    QAbstractItemModel* model = table->model();
    const int before = model->rowCount();
    const QString groupOfFirst = model->index(0, QuickCommandModel::Group).data(Qt::EditRole).toString();

    dialog.setCurrentRow(0);
    child<QPushButton>(&dialog, "addButton")->click();

    // Inserted right after the current row, inheriting its group, and selected for editing.
    QCOMPARE(model->rowCount(), before + 1);
    QCOMPARE(cellText(model, 1, QuickCommandModel::Name), QStringLiteral("New command"));
    QCOMPARE(model->index(1, QuickCommandModel::Group).data(Qt::EditRole).toString(), groupOfFirst);
    QCOMPARE(table->currentIndex().row(), 1);
    QVERIFY(store.commands().size() == before);   // nothing written until OK
}

void Tst_dialogs::dialogRemoveButton()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("remove.json"))));
    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");
    QAbstractItemModel* model = table->model();
    const int before = model->rowCount();
    const QString first = cellText(model, 0, QuickCommandModel::Name);
    const QString second = cellText(model, 1, QuickCommandModel::Name);

    dialog.setCurrentRow(0);
    child<QPushButton>(&dialog, "removeButton")->click();
    QCOMPARE(model->rowCount(), before - 1);
    QCOMPARE(cellText(model, 0, QuickCommandModel::Name), second);
    QVERIFY(cellText(model, 0, QuickCommandModel::Name) != first);
    QCOMPARE(table->currentIndex().row(), 0);

    // Removing the last row moves the selection up.
    dialog.setCurrentRow(model->rowCount() - 1);
    child<QPushButton>(&dialog, "removeButton")->click();
    QCOMPARE(model->rowCount(), before - 2);
    QCOMPARE(table->currentIndex().row(), before - 3);
    QCOMPARE(store.commands().size(), before);
}

void Tst_dialogs::dialogMoveButtons()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("move.json"))));
    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");
    QAbstractItemModel* model = table->model();
    QPushButton* up = child<QPushButton>(&dialog, "moveUpButton");
    QPushButton* down = child<QPushButton>(&dialog, "moveDownButton");
    const QString row0 = cellText(model, 0, QuickCommandModel::Name);
    const QString row1 = cellText(model, 1, QuickCommandModel::Name);
    QVERIFY(row0 != row1);

    dialog.setCurrentRow(1);
    QVERIFY(up->isEnabled());
    up->click();
    QCOMPARE(cellText(model, 0, QuickCommandModel::Name), row1);
    QCOMPARE(cellText(model, 1, QuickCommandModel::Name), row0);
    QCOMPARE(table->currentIndex().row(), 0);
    QVERIFY(!up->isEnabled());   // already at the top
    up->click();                 // no-op
    QCOMPARE(cellText(model, 0, QuickCommandModel::Name), row1);

    QVERIFY(down->isEnabled());
    down->click();
    QCOMPARE(cellText(model, 0, QuickCommandModel::Name), row0);
    QCOMPARE(cellText(model, 1, QuickCommandModel::Name), row1);
    QCOMPARE(table->currentIndex().row(), 1);

    dialog.setCurrentRow(model->rowCount() - 1);
    QVERIFY(!down->isEnabled());
    down->click();   // no-op at the bottom
    QCOMPARE(table->currentIndex().row(), model->rowCount() - 1);
}

void Tst_dialogs::dialogButtonEnableState()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("enable.json"))));
    {
        QuickCommandsDialog dialog(&store);
        QVERIFY(expose(&dialog));
        QVERIFY(child<QPushButton>(&dialog, "addButton")->isEnabled());
        QVERIFY(child<QPushButton>(&dialog, "removeButton")->isEnabled());
        QVERIFY(!child<QPushButton>(&dialog, "moveUpButton")->isEnabled());
        QVERIFY(child<QPushButton>(&dialog, "moveDownButton")->isEnabled());
        QVERIFY(child<QPushButton>(&dialog, "exportButton")->isEnabled());
        QVERIFY(child<QPushButton>(&dialog, "importButton")->isEnabled());
        QVERIFY(child<QPushButton>(&dialog, "restoreDefaultsButton")->isEnabled());

        auto* table = child<QTableView>(&dialog, "tableView");
        dialog.setCurrentRow(table->model()->rowCount() - 1);
        QVERIFY(child<QPushButton>(&dialog, "moveUpButton")->isEnabled());
        QVERIFY(!child<QPushButton>(&dialog, "moveDownButton")->isEnabled());
    }

    // An empty list: nothing to remove, move or export until a row is added.
    QuickCommandStore empty;
    empty.setCommands({});
    QuickCommandsDialog dialog(&empty);
    QVERIFY(expose(&dialog));
    QVERIFY(child<QPushButton>(&dialog, "addButton")->isEnabled());
    QVERIFY(!child<QPushButton>(&dialog, "removeButton")->isEnabled());
    QVERIFY(!child<QPushButton>(&dialog, "moveUpButton")->isEnabled());
    QVERIFY(!child<QPushButton>(&dialog, "moveDownButton")->isEnabled());
    QVERIFY(!child<QPushButton>(&dialog, "exportButton")->isEnabled());

    child<QPushButton>(&dialog, "addButton")->click();
    QCOMPARE(child<QTableView>(&dialog, "tableView")->model()->rowCount(), 1);
    QVERIFY(child<QPushButton>(&dialog, "removeButton")->isEnabled());
    QVERIFY(child<QPushButton>(&dialog, "exportButton")->isEnabled());
    QVERIFY(!child<QPushButton>(&dialog, "moveUpButton")->isEnabled());
    QVERIFY(!child<QPushButton>(&dialog, "moveDownButton")->isEnabled());
}

void Tst_dialogs::dialogSetCurrentRow()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("current.json"))));
    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");

    dialog.setCurrentRow(3);
    QCOMPARE(table->currentIndex().row(), 3);
    QVERIFY(table->selectionModel()->isRowSelected(3, QModelIndex()));
    dialog.setCurrentRow(-1);   // ignored
    QCOMPARE(table->currentIndex().row(), 3);
    dialog.setCurrentRow(table->model()->rowCount());   // ignored
    QCOMPARE(table->currentIndex().row(), 3);
}

void Tst_dialogs::dialogAcceptWritesStoreAndSavesJson()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("accept.json"))));
    const int before = store.commands().size();
    QSignalSpy changed(&store, &QuickCommandStore::changed);
    const QString savedPath = QuickCommandStore::defaultFilePath();
    QFile::remove(savedPath);
    QVERIFY(!QFileInfo::exists(savedPath));

    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");
    dialog.setCurrentRow(0);
    child<QPushButton>(&dialog, "addButton")->click();
    QVERIFY(table->model()->setData(table->model()->index(1, QuickCommandModel::Command), QStringLiteral("added cmd")));
    QCOMPARE(table->model()->rowCount(), before + 1);

    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(!dialog.isVisible());

    // The store received the edited list...
    QCOMPARE(changed.count(), 1);
    QCOMPARE(store.commands().size(), before + 1);
    QCOMPARE(store.commands().at(1).name, QStringLiteral("New command"));
    QCOMPARE(store.commands().at(1).command, QStringLiteral("added cmd"));

    // ...and saved it as JSON in the (test-mode) data directory.
    QVERIFY2(QFileInfo::exists(savedPath), qPrintable(savedPath));
    QVERIFY(savedPath.contains(QStringLiteral("SerialUtilityTest-dialogs")));
    QuickCommandStore reloaded;
    QVERIFY(reloaded.load(savedPath));
    QVERIFY(reloaded.commands() == store.commands());
}

void Tst_dialogs::dialogCancelKeepsStore()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("cancel.json"))));
    const QList<QuickCommand> original = store.commands();
    QSignalSpy changed(&store, &QuickCommandStore::changed);

    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    dialog.setCurrentRow(0);
    child<QPushButton>(&dialog, "removeButton")->click();
    child<QPushButton>(&dialog, "removeButton")->click();
    QCOMPARE(child<QTableView>(&dialog, "tableView")->model()->rowCount(), original.size() - 2);

    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    QVERIFY(!dialog.isVisible());
    QCOMPARE(changed.count(), 0);
    QVERIFY(store.commands() == original);
}

void Tst_dialogs::dialogRestoreDefaultsConfirmed()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("restore-yes.json"))));
    const QList<QuickCommand> defaults = QuickCommandStore::defaults();
    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");
    dialog.setCurrentRow(0);
    child<QPushButton>(&dialog, "removeButton")->click();
    child<QPushButton>(&dialog, "removeButton")->click();
    QCOMPARE(table->model()->rowCount(), defaults.size() - 2);

    ModalDismisser dismisser(ModalDismisser::Answer::Yes);
    child<QPushButton>(&dialog, "restoreDefaultsButton")->click();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);   // the confirmation question

    QCOMPARE(table->model()->rowCount(), defaults.size());
    QCOMPARE(cellText(table->model(), 0, QuickCommandModel::Name), defaults.first().name);
    QCOMPARE(table->currentIndex().row(), 0);
    QVERIFY(store.commands() == defaults);   // not written yet (already equal here) ...
    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok)->click();
    QVERIFY(store.commands() == defaults);
}

void Tst_dialogs::dialogRestoreDefaultsDeclined()
{
    QuickCommandStore store;
    QVERIFY(store.load(tempPath(QStringLiteral("restore-no.json"))));
    QuickCommandsDialog dialog(&store);
    QVERIFY(expose(&dialog));
    auto* table = child<QTableView>(&dialog, "tableView");
    const int before = table->model()->rowCount();
    dialog.setCurrentRow(0);
    child<QPushButton>(&dialog, "removeButton")->click();
    const QString firstAfterRemove = cellText(table->model(), 0, QuickCommandModel::Name);

    ModalDismisser dismisser(ModalDismisser::Answer::No);
    child<QPushButton>(&dialog, "restoreDefaultsButton")->click();
    dismisser.stop();
    QCOMPARE(dismisser.count(), 1);

    QCOMPARE(table->model()->rowCount(), before - 1);   // unchanged by the declined question
    QCOMPARE(cellText(table->model(), 0, QuickCommandModel::Name), firstAfterRemove);
}

// =======================================================================================
// QuickCommandStore import / export
// =======================================================================================

void Tst_dialogs::storeExportImportRoundTrip()
{
    QuickCommand hex = makeCommand(QStringLiteral("Ctrl+C"), QStringLiteral("03"), QStringLiteral("Control"));
    hex.hex = true;
    QuickCommand escaped = makeCommand(QStringLiteral("AT"), QStringLiteral("AT\\r\\n"), QStringLiteral("MCU"),
                                       LineEnding::Mode::None);
    escaped.escapes = true;
    escaped.shortcut = QStringLiteral("Ctrl+1");
    escaped.tooltip = QStringLiteral("attention");
    const QList<QuickCommand> commands = {
        makeCommand(QStringLiteral("uname"), QStringLiteral("uname -a"), QStringLiteral("Linux")), hex, escaped};

    QuickCommandStore source;
    source.setCommands(commands);
    const QString path = tempPath(QStringLiteral("export/quick_commands.json"));   // directory is created
    QString error;
    QVERIFY2(source.exportToFile(path, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo::exists(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object().value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(doc.object().value(QStringLiteral("commands")).toArray().size(), 3);

    QuickCommandStore target;
    target.setCommands({makeCommand(QStringLiteral("stale"), QStringLiteral("stale"), QString())});
    QSignalSpy changed(&target, &QuickCommandStore::changed);
    QVERIFY2(target.importFromFile(path, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(changed.count(), 1);
    QVERIFY(target.commands() == commands);
    QCOMPARE(target.groups(), (QStringList{QStringLiteral("Linux"), QStringLiteral("Control"), QStringLiteral("MCU")}));

    // Exporting onto a directory fails with a message.
    QVERIFY(!source.exportToFile(m_tempDir.path(), &error));
    QVERIFY(!error.isEmpty());
}

void Tst_dialogs::storeImportErrors()
{
    QuickCommandStore store;
    const QList<QuickCommand> original = {makeCommand(QStringLiteral("keep"), QStringLiteral("keep"), QString())};
    store.setCommands(original);
    QSignalSpy changed(&store, &QuickCommandStore::changed);
    QString error;

    QVERIFY(!store.importFromFile(tempPath(QStringLiteral("does-not-exist.json")), &error));
    QVERIFY2(error.contains(QStringLiteral("does-not-exist.json")), qPrintable(error));

    const QString malformed = tempPath(QStringLiteral("malformed.json"));
    QVERIFY(writeFile(malformed, QByteArrayLiteral("{ \"commands\": [ oops")));
    QVERIFY(!store.importFromFile(malformed, &error));
    QVERIFY(!error.isEmpty());

    const QString noArray = tempPath(QStringLiteral("no-array.json"));
    QVERIFY(writeFile(noArray, QByteArrayLiteral("{ \"version\": 1 }")));
    QVERIFY(!store.importFromFile(noArray, &error));
    QVERIFY2(error.contains(QStringLiteral("commands")), qPrintable(error));

    const QString notObject = tempPath(QStringLiteral("not-object.json"));
    QVERIFY(writeFile(notObject, QByteArrayLiteral("[ ]")));
    QVERIFY(!store.importFromFile(notObject, &error));
    QVERIFY(!error.isEmpty());

    QCOMPARE(changed.count(), 0);
    QVERIFY(store.commands() == original);

    // A minimal valid document imports; missing fields take the documented defaults.
    const QString valid = tempPath(QStringLiteral("valid.json"));
    QVERIFY(writeFile(valid, QByteArrayLiteral("{ \"version\": 1, \"commands\": [ { \"name\": \"ls\", \"command\": "
                                                "\"ls -l\" } ] }")));
    QVERIFY2(store.importFromFile(valid, &error), qPrintable(error));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(store.commands().size(), 1);
    QCOMPARE(store.commands().first().name, QStringLiteral("ls"));
    QCOMPARE(store.commands().first().command, QStringLiteral("ls -l"));
    QCOMPARE(static_cast<int>(store.commands().first().lineEnding), static_cast<int>(LineEnding::Mode::CR));
    QVERIFY(!store.commands().first().hex);
    QVERIFY(!store.commands().first().escapes);
}

// =======================================================================================
// SendFileDialog
// =======================================================================================

void Tst_dialogs::sendFileBackpressure()
{
    const QString path = tempPath(QStringLiteral("backpressure.bin"));
    QByteArray blob(64 * 1024, '\0');
    for (qsizetype i = 0; i < blob.size(); ++i) {
        blob[i] = static_cast<char>(i & 0xFF);
    }
    QVERIFY(writeFile(path, blob));

    SendFileDialog dialog;
    QVERIFY(expose(&dialog));
    dialog.setConnected(true);
    dialog.setFilePath(path);
    auto* binaryRadio = child<QRadioButton>(&dialog, "modeBinaryRadio");
    auto* chunkSize = child<QSpinBox>(&dialog, "chunkSizeSpin");
    auto* chunkDelay = child<QSpinBox>(&dialog, "chunkDelaySpin");
    auto* startButton = child<QPushButton>(&dialog, "startButton");
    auto* pauseButton = child<QPushButton>(&dialog, "pauseButton");
    auto* progressBar = child<QProgressBar>(&dialog, "progressBar");
    auto* statusLabel = child<QLabel>(&dialog, "statusLabel");
    QVERIFY(binaryRadio && chunkSize && chunkDelay && startButton && pauseButton && progressBar && statusLabel);
    binaryRadio->setChecked(true);
    chunkSize->setValue(256);
    chunkDelay->setValue(0);
    QCOMPARE(dialog.options().mode, FileSender::Mode::Binary);
    QCOMPARE(dialog.options().chunkSize, 256);

    qint64 sentBytes = 0;
    connect(&dialog, &SendFileDialog::sendChunk, this,
            [&sentBytes](const QByteArray& chunk) { sentBytes += chunk.size(); });
    QSignalSpy finishedSpy(&dialog, &SendFileDialog::sendingFinished);

    // 1) A congested port (far more than two chunks queued) holds the sender before its first chunk.
    QVERIFY(startButton->isEnabled());
    startButton->click();
    QVERIFY(dialog.isSending());
    dialog.updatePendingTx(100000);
    QVERIFY(dialog.isSending());
    QCOMPARE(pauseButton->text(), QStringLiteral("&Pause"));   // the hold is not a user pause
    QVERIFY2(statusLabel->text().contains(QStringLiteral("drain")), qPrintable(statusLabel->text()));
    QTest::qWait(100);
    QCOMPARE(sentBytes, qint64(0));
    QCOMPARE(progressBar->value(), 0);
    QVERIFY(dialog.isSending());
    QCOMPARE(finishedSpy.count(), 0);

    // Still above one chunk: the hold stays.
    dialog.updatePendingTx(300);
    QTest::qWait(50);
    QCOMPARE(sentBytes, qint64(0));

    // Drained: the transfer resumes and runs to completion.
    dialog.updatePendingTx(0);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    QCOMPARE(sentBytes, qint64(blob.size()));
    QCOMPARE(progressBar->value(), 100);
    QVERIFY(!dialog.isSending());
    QVERIFY(startButton->isEnabled());

    // 2) A user Pause wins: a drained port must not resume it.
    sentBytes = 0;
    finishedSpy.clear();
    startButton->click();
    QVERIFY(dialog.isSending());
    pauseButton->click();
    QCOMPARE(pauseButton->text(), QStringLiteral("&Resume"));
    dialog.updatePendingTx(0);
    QTest::qWait(100);
    QCOMPARE(pauseButton->text(), QStringLiteral("&Resume"));
    QVERIFY(dialog.isSending());
    QCOMPARE(sentBytes, qint64(0));
    QCOMPARE(finishedSpy.count(), 0);
    pauseButton->click();   // Resume
    QCOMPARE(pauseButton->text(), QStringLiteral("&Pause"));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    QCOMPARE(sentBytes, qint64(blob.size()));

    // 3) Bytes still queued when the last chunk was handed over: 99% until the port reports 0.
    sentBytes = 0;
    finishedSpy.clear();
    const QMetaObject::Connection tail =
        connect(&dialog, &SendFileDialog::sendChunk, &dialog, [&dialog]() { dialog.updatePendingTx(100); });
    startButton->click();
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 10000);
    QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    QCOMPARE(sentBytes, qint64(blob.size()));
    QCOMPARE(progressBar->value(), 99);
    QVERIFY2(statusLabel->text().contains(QStringLiteral("still leaving the port")), qPrintable(statusLabel->text()));
    disconnect(tail);
    dialog.updatePendingTx(0);
    QCOMPARE(progressBar->value(), 100);
    QVERIFY2(!statusLabel->text().contains(QStringLiteral("still leaving")), qPrintable(statusLabel->text()));
    QVERIFY(!dialog.isSending());
}

// The dialog is long-lived (SessionWidget reuses it for the lifetime of the tab), so it must
// retranslate itself on QEvent::LanguageChange, and a running transfer must keep its progress
// text instead of falling back to the .ui default "Ready.".
void Tst_dialogs::sendFileRetranslates()
{
    const QString path = tempPath(QStringLiteral("retranslate.bin"));
    QVERIFY(writeFile(path, QByteArray(16 * 1024, 'x')));

    SendFileDialog dialog;
    QVERIFY(expose(&dialog));
    dialog.setConnected(true);
    dialog.setFilePath(path);
    auto* startButton = child<QPushButton>(&dialog, "startButton");
    auto* pauseButton = child<QPushButton>(&dialog, "pauseButton");
    auto* statusLabel = child<QLabel>(&dialog, "statusLabel");
    auto* binaryRadio = child<QRadioButton>(&dialog, "modeBinaryRadio");
    QVERIFY(startButton && pauseButton && statusLabel && binaryRadio);
    binaryRadio->setChecked(true);

    const QString englishTitle = QStringLiteral("Send File");
    const QString englishStart = QStringLiteral("&Start");
    const QString englishReady = QStringLiteral("Ready.");
    QCOMPARE(dialog.windowTitle(), englishTitle);
    QCOMPARE(startButton->text(), englishStart);
    QCOMPARE(statusLabel->text(), englishReady);

    QTranslator translator;
    QVERIFY(translator.load(QStringLiteral(":/translations/zh_CN.qm")));

    // 1) Idle: every string flips to Chinese and back.
    QVERIFY(qApp->installTranslator(&translator));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QVERIFY2(dialog.windowTitle() != englishTitle, qPrintable(dialog.windowTitle()));
    QVERIFY2(startButton->text() != englishStart, qPrintable(startButton->text()));
    QVERIFY2(statusLabel->text() != englishReady, qPrintable(statusLabel->text()));
    const QString chineseTitle = dialog.windowTitle();
    const QString chineseReady = statusLabel->text();

    QVERIFY(qApp->removeTranslator(&translator));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCOMPARE(dialog.windowTitle(), englishTitle);
    QCOMPARE(startButton->text(), englishStart);
    QCOMPARE(statusLabel->text(), englishReady);

    // 2) While a (paused) transfer is running the progress text survives the switch.
    startButton->click();
    QVERIFY(dialog.isSending());
    pauseButton->click();
    QCOMPARE(pauseButton->text(), QStringLiteral("&Resume"));

    QVERIFY(qApp->installTranslator(&translator));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QVERIFY(dialog.isSending());
    QCOMPARE(dialog.windowTitle(), chineseTitle);
    QVERIFY2(pauseButton->text() != QStringLiteral("&Resume"), qPrintable(pauseButton->text()));
    QVERIFY2(statusLabel->text() != englishReady, qPrintable(statusLabel->text()));
    QVERIFY2(statusLabel->text() != chineseReady, qPrintable(statusLabel->text()));
    QVERIFY2(statusLabel->text().contains(QStringLiteral(" / ")), qPrintable(statusLabel->text()));   // "a / b (n%)"

    QVERIFY(qApp->removeTranslator(&translator));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCOMPARE(dialog.windowTitle(), englishTitle);
    QCOMPARE(pauseButton->text(), QStringLiteral("&Resume"));
    QVERIFY2(statusLabel->text().contains(QStringLiteral("[paused]")), qPrintable(statusLabel->text()));

    child<QPushButton>(&dialog, "cancelButton")->click();
    QVERIFY(!dialog.isSending());
}

// =======================================================================================
// AboutDialog / VersionDialog
// =======================================================================================

// Both help dialogs are modeless and can stay open across Language > ...: they retranslate.
void Tst_dialogs::helpDialogsRetranslate()
{
    AboutDialog about;
    VersionDialog version;
    QVERIFY(expose(&about));
    QVERIFY(expose(&version));
    const QString englishAbout = about.windowTitle();
    const QString englishVersion = QStringLiteral("Version Information");
    QVERIFY(englishAbout.startsWith(QStringLiteral("About ")));
    QCOMPARE(version.windowTitle(), englishVersion);
    auto* versionLabel = child<QLabel>(&version, "labelAppVersion");
    QVERIFY(versionLabel);
    const QString englishVersionLabel = versionLabel->text();
    QVERIFY(englishVersionLabel.contains(QStringLiteral("Application Version:")));

    QTranslator translator;
    QVERIFY(translator.load(QStringLiteral(":/translations/zh_CN.qm")));
    QVERIFY(qApp->installTranslator(&translator));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QVERIFY2(about.windowTitle() != englishAbout, qPrintable(about.windowTitle()));
    QVERIFY2(about.windowTitle().contains(QStringLiteral(APP_DISPLAY_NAME)), qPrintable(about.windowTitle()));
    QVERIFY2(version.windowTitle() != englishVersion, qPrintable(version.windowTitle()));
    QVERIFY2(versionLabel->text() != englishVersionLabel, qPrintable(versionLabel->text()));
    QVERIFY2(versionLabel->text().contains(QStringLiteral(APP_VERSION)), qPrintable(versionLabel->text()));

    QVERIFY(qApp->removeTranslator(&translator));
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QCOMPARE(about.windowTitle(), englishAbout);
    QCOMPARE(version.windowTitle(), englishVersion);
    QCOMPARE(versionLabel->text(), englishVersionLabel);
}

void Tst_dialogs::aboutDialogContent()
{
    AboutDialog dialog;
    QVERIFY(expose(&dialog));
    QVERIFY(!dialog.isModal());
    QVERIFY(dialog.windowTitle().contains(QStringLiteral("About")));
    QVERIFY(dialog.windowTitle().contains(QStringLiteral(APP_DISPLAY_NAME)));

    auto* name = child<QLabel>(&dialog, "labelAppName");
    QVERIFY(name);
    QVERIFY2(name->text().contains(QStringLiteral(APP_DISPLAY_NAME)), qPrintable(name->text()));
    QVERIFY2(name->text().contains(QStringLiteral(APP_VERSION)), qPrintable(name->text()));
    QVERIFY(child<QLabel>(&dialog, "labelLogo") != nullptr);

    auto* description = child<QTextBrowser>(&dialog, "textDescription");
    auto* credits = child<QTextBrowser>(&dialog, "textCredits");
    auto* license = child<QTextBrowser>(&dialog, "textLicense");
    QVERIFY(description);
    QVERIFY(credits);
    QVERIFY(license);
    QVERIFY(description->toPlainText().contains(QStringLiteral("Rockchip")));
    QVERIFY(description->toPlainText().contains(QStringLiteral(APP_HOMEPAGE)));
    QVERIFY(description->openExternalLinks());
    QVERIFY2(credits->toPlainText().contains(QString::fromLatin1(qVersion())), qPrintable(credits->toPlainText()));
    QVERIFY(credits->toPlainText().contains(QStringLiteral("C++20")));
    QVERIFY(license->toPlainText().contains(QStringLiteral("Copyright")));
    QVERIFY(license->toPlainText().contains(QStringLiteral("BuildAI")));
}

void Tst_dialogs::aboutDialogLogoResource()
{
    // AboutDialog.h: the logo is :/icons/buildai_64.png. resources/resources.qrc is listed as a
    // plain source of the executables, which only works with CMAKE_AUTORCC; qt_standard_project_setup()
    // enables AUTOMOC / AUTOUIC but not AUTORCC, so the icons are missing from every binary.
    if (!QFile::exists(QStringLiteral(":/icons/buildai_64.png"))) {
        QSKIP("resources.qrc is not compiled in: add set(CMAKE_AUTORCC ON) after qt_standard_project_setup() "
              "(or qt_add_resources) in CMakeLists.txt so :/icons/* exist at run time");
    }
    AboutDialog dialog;
    QVERIFY(expose(&dialog));
    auto* logo = child<QLabel>(&dialog, "labelLogo");
    QVERIFY(logo);
    QVERIFY(!logo->pixmap().isNull());
    QCOMPARE(logo->pixmap().size(), QSize(64, 64));
}

void Tst_dialogs::aboutDialogCloseButton()
{
    AboutDialog dialog;
    QVERIFY(expose(&dialog));
    auto* buttons = child<QDialogButtonBox>(&dialog, "buttonBox");
    QVERIFY(buttons);
    QPushButton* close = buttons->button(QDialogButtonBox::Close);
    QVERIFY(close);
    close->click();
    QTRY_VERIFY(!dialog.isVisible());
}

void Tst_dialogs::versionDialogContent()
{
    VersionDialog dialog;
    QVERIFY(expose(&dialog));
    QVERIFY(!dialog.isModal());
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Version Information"));
    QCOMPARE(child<QLabel>(&dialog, "labelTitle")->text(), QStringLiteral(APP_DISPLAY_NAME));
    QVERIFY(child<QLabel>(&dialog, "labelAppVersion")->text().contains(QStringLiteral(APP_VERSION)));
    QVERIFY(child<QLabel>(&dialog, "labelGitHash")->text().contains(QStringLiteral(APP_GIT_HASH)));
    QVERIFY(child<QLabel>(&dialog, "labelBuildDate")->text().contains(QStringLiteral(APP_BUILD_DATE)));
    QVERIFY(child<QLabel>(&dialog, "labelQtVersion")->text().contains(QString::fromLatin1(qVersion())));
    QVERIFY(child<QLabel>(&dialog, "labelCompiler")->text().contains(VersionDialog::compilerInfo()));
    QVERIFY(child<QLabel>(&dialog, "labelBuildType")->text().contains(VersionDialog::buildType()));
    QVERIFY(!child<QLabel>(&dialog, "labelOS")->text().isEmpty());
    QVERIFY(!child<QLabel>(&dialog, "labelArchitecture")->text().isEmpty());
    auto* additional = child<QTextBrowser>(&dialog, "textAdditionalInfo");
    QVERIFY(additional);
    QVERIFY(additional->toPlainText().contains(QStringLiteral("Qt6::SerialPort")));
    QVERIFY(additional->toPlainText().contains(QStringLiteral("UTF-8")));

#if defined(_MSC_VER)
    QVERIFY(VersionDialog::compilerInfo().contains(QStringLiteral("MSVC")));
#endif
#if defined(QT_NO_DEBUG)
    QCOMPARE(VersionDialog::buildType(), QStringLiteral("Release"));
#else
    QCOMPARE(VersionDialog::buildType(), QStringLiteral("Debug"));
#endif

    const QString report = dialog.plainTextReport();
    QVERIFY(!report.isEmpty());
    QVERIFY(report.startsWith(QStringLiteral(APP_DISPLAY_NAME)));
    QVERIFY(report.contains(QStringLiteral(APP_VERSION)));
    QVERIFY(report.contains(QString::fromLatin1(qVersion())));
    QVERIFY(report.contains(QStringLiteral(APP_GIT_HASH)));
    QVERIFY(report.contains(VersionDialog::compilerInfo()));
    QVERIFY(report.contains(VersionDialog::buildType()));
    QVERIFY(report.contains(QStringLiteral("UTF-8")));
    QCOMPARE(report.split(QLatin1Char('\n')).size(), 11);
}

void Tst_dialogs::versionDialogCopyButton()
{
    VersionDialog dialog;
    QVERIFY(expose(&dialog));
    auto* copy = child<QPushButton>(&dialog, "buttonCopy");
    QVERIFY(copy);
    const QString before = copy->text();
    QVERIFY(QApplication::clipboard()->text().isEmpty());

    copy->click();
    QCOMPARE(QApplication::clipboard()->text(), dialog.plainTextReport());
    QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral(APP_VERSION)));
    QCOMPARE(copy->text(), QStringLiteral("Copied"));
    QVERIFY(copy->text() != before);
}

void Tst_dialogs::versionDialogCloseButton()
{
    VersionDialog dialog;
    QVERIFY(expose(&dialog));
    auto* buttons = child<QDialogButtonBox>(&dialog, "buttonBox");
    QVERIFY(buttons);
    QPushButton* close = buttons->button(QDialogButtonBox::Close);
    QVERIFY(close);
    close->click();
    QTRY_VERIFY(!dialog.isVisible());
}

QTEST_MAIN(Tst_dialogs)
#include "tst_dialogs.moc"
