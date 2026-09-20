// GUI test suite for SessionWidget and its control strips (ConnectionBar, CommandInput,
// QuickCommandBar) driven over the built-in simulated devices (SIM:loopback, SIM:linux) and
// the log replayer. Runs offscreen (QT_QPA_PLATFORM=offscreen); all settings and files live
// in a temporary directory, nothing touches the user's real configuration.
#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIntValidator>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTranslator>

#include <memory>

#include "app/AppSettings.h"
#include "core/CommandHistory.h"
#include "core/LineEnding.h"
#include "core/QuickCommand.h"
#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"
#include "core/SessionLogger.h"
#include "dialogs/SendFileDialog.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWidget.h"
#include "ui/CommandInput.h"
#include "ui/ConnectionBar.h"
#include "ui/HexDumpView.h"
#include "ui/QuickCommandBar.h"
#include "ui/SessionWidget.h"

namespace {

using State = SerialConnection::State;

constexpr int kSimTimeoutMs = 10000;  ///< simulated device round trips / reconnects
constexpr int kBootTimeoutMs = 15000; ///< a simulated Linux boot log up to "login:"

const QString kLoopback = QStringLiteral("SIM:loopback");
const QString kLinux = QStringLiteral("SIM:linux");
const QString kGroupKey = QStringLiteral("ui/quickCommandGroup");

/// Every line of the terminal (scrollback followed by the visible screen), joined with '\n'.
QString allText(const TerminalWidget* terminal)
{
    const TerminalScreen* screen = terminal->screen();
    QStringList lines;
    const int total = screen->totalLines();
    lines.reserve(total);
    for (int i = 0; i < total; ++i) {
        lines.append(screen->lineText(i));
    }
    return lines.join(QLatin1Char('\n'));
}

/// The visible rows only, joined with '\n'.
QString visibleText(const TerminalWidget* terminal)
{
    const TerminalScreen* screen = terminal->screen();
    QStringList lines;
    lines.reserve(screen->rows());
    for (int i = 0; i < screen->rows(); ++i) {
        lines.append(screen->lineText(screen->scrollbackSize() + i));
    }
    return lines.join(QLatin1Char('\n'));
}

/// Text of the first visible row (where a fresh terminal's cursor sits).
QString firstRow(const TerminalWidget* terminal)
{
    const TerminalScreen* screen = terminal->screen();
    return screen->lineText(screen->scrollbackSize());
}

/// The command buttons of a QuickCommandBar in layout order, after pending deleteLater()s ran.
QList<QToolButton*> commandButtons(QuickCommandBar* bar)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return bar->findChildren<QToolButton*>(QStringLiteral("quickCommandButton"));
}

QStringList buttonTexts(const QList<QToolButton*>& buttons)
{
    QStringList texts;
    for (const QToolButton* button : buttons) {
        texts.append(button->text());
    }
    return texts;
}

QStringList commandNames(const QList<QuickCommand>& commands, const QString& group = QString())
{
    QStringList names;
    for (const QuickCommand& qc : commands) {
        if (group.isEmpty() || qc.group == group) {
            names.append(qc.name);
        }
    }
    return names;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

template <typename T>
T* child(const QObject* parent, const char* objectName)
{
    return parent->findChild<T*>(QString::fromLatin1(objectName));
}

SerialPortEntry makeEntry(const QString& port, const QString& description = QString())
{
    SerialPortEntry entry;
    entry.portName = port;
    entry.description = description;
    return entry;
}

QuickCommand makeCommand(const QString& name, const QString& command, const QString& group)
{
    QuickCommand qc;
    qc.name = name;
    qc.command = command;
    qc.group = group;
    return qc;
}

/// Show a top-level widget offscreen and wait until it is exposed.
bool expose(QWidget* widget)
{
    widget->show();
    return QTest::qWaitForWindowExposed(widget);
}

} // namespace

class Tst_sessionwidget : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- SessionWidget over SIM:loopback --------------------------------------------
    void loopbackConnect();
    void loopbackTerminalTyping();
    void loopbackCommandInputSend();
    void loopbackHexModeRoundTrip();
    void loopbackEscapeMode();
    void loopbackEscapeModeGbk();
    void loopbackEscapeModeLatin1();
    void loopbackHistoryNavigation();
    void loopbackQuickCommand();
    void loopbackQuickCommandBarClick();
    void loopbackLogging();
    void loopbackLoggingRawFormat();
    void loopbackAutoLogOnConnect();
    void autoLogSwitchesFileOnPortChange();
    void manualLogKeptAcrossPortChange();
    void loopbackViewModeSwitch();
    void loopbackDisconnect();
    void loopbackSendFileDialog();
    void loopbackSyncTerminalSize();
    void loopbackLiveBaudChange();
    void loopbackCounters();
    void loopbackConnectWithoutPort();
    void loopbackToggleConnection();
    void loopbackClearTerminal();
    void loopbackSendBreak();

    // ---- SessionWidget over SIM:linux -----------------------------------------------
    void linuxLoginUnameReboot();

    // ---- Log replay -----------------------------------------------------------------
    void replayRawFile();
    void replayRefusedWhileConnected();
    void replayStopMidway();
    void connectDuringReplayStopsReplay();
    void replayTimestampedCapture();

    // ---- ConnectionBar --------------------------------------------------------------
    void barSetPorts();
    void barSelectMissingPortPlaceholder();
    void barSettingsRoundTrip();
    void barConnectionStateUi();
    void barPinToggles();
    void barBreakButton();
    void barSettingsChangedOnlyByUser();
    void barButtonsEmitRequests();

    // ---- CommandInput ---------------------------------------------------------------
    void inputInvalidHex();
    void inputHexModeDisablesControls();
    void inputCtrlLClears();
    void inputEscapeClears();
    void inputHistoryDraftRestore();
    void inputEnterDisconnectedNoEmit();
    void inputLineEndingChanged();
    void inputEscapePayload();

    // ---- QuickCommandBar ------------------------------------------------------------
    void quickBarButtonsMatchGroup();
    void quickBarClickEmits();
    void quickBarGroupFilterPersists();
    void quickBarGroupTranslated();
    void quickBarGearEmitsEdit();
    void quickBarRebuildOnStoreChanged();
    void quickBarEnabledForConnection();

private:
    std::unique_ptr<SessionWidget> newSession();
    bool connectTo(SessionWidget* session, const QString& port);
    QString tempPath(const QString& name) const;
    static bool roundTrips(ConnectionBar& bar, const SerialSettings& settings);

    QTemporaryDir m_tempDir;
    QuickCommandStore* m_store = nullptr;
    CommandHistory m_history;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_sessionwidget::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-session"));
    QVERIFY(m_tempDir.isValid());

    // AppSettings uses the default QSettings() constructor: keep it out of the registry and
    // inside the temporary directory, and make sure it picked up the test names above.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("BuildAI-Test")));
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-session")));

    AppSettings& app = AppSettings::instance();
    app.setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    app.setAutoReconnect(true);
    app.setReconnectIntervalMs(200);
    app.setShowSimulatedPorts(true);
    app.setAutoLog(false);
    app.setLogFormat(QStringLiteral("text"));
    app.setLogIncludeTx(true);
    QCOMPARE(app.logDirectory(), m_tempDir.filePath(QStringLiteral("logs")));
    QCOMPARE(app.reconnectIntervalMs(), 200);
    QVERIFY(settings.contains(QStringLiteral("connection/reconnectIntervalMs")));

    m_store = new QuickCommandStore(this);
    // A missing file loads the defaults; nothing is ever saved to the real data directory.
    QVERIFY(m_store->load(m_tempDir.filePath(QStringLiteral("quick_commands.json"))));
    QVERIFY(m_store->commands() == QuickCommandStore::defaults());
    QVERIFY(m_store->groups().contains(QStringLiteral("Linux")));
}

void Tst_sessionwidget::init()
{
    m_history.clear();
    m_store->setCommands(QuickCommandStore::defaults());
    QSettings().remove(kGroupKey);
    AppSettings& app = AppSettings::instance();
    app.setAutoLog(false);
    app.setLogFormat(QStringLiteral("text"));
    app.setLogIncludeTx(true);
    app.setEncoding(QStringLiteral("UTF-8"));
}

std::unique_ptr<SessionWidget> Tst_sessionwidget::newSession()
{
    auto session = std::make_unique<SessionWidget>(m_store, &m_history);
    session->resize(1000, 700);
    if (!expose(session.get())) {
        return nullptr;
    }
    session->activateWindow();
    return session;
}

bool Tst_sessionwidget::connectTo(SessionWidget* session, const QString& port)
{
    session->setPortName(port);
    if (session->portName() != port) {
        qWarning() << "port not selected:" << session->portName();
        return false;
    }
    if (!session->connectPort()) {
        qWarning() << "connectPort() failed for" << port;
        return false;
    }
    session->terminal()->setFocus();
    return session->isConnected();
}

QString Tst_sessionwidget::tempPath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

bool Tst_sessionwidget::roundTrips(ConnectionBar& bar, const SerialSettings& settings)
{
    bar.setSettings(settings);
    const SerialSettings back = bar.settings();
    if (back != settings) {
        qWarning() << "settings round trip failed: wanted" << settings.portName << settings.summary() << settings.dtr
                   << settings.rts << "got" << back.portName << back.summary() << back.dtr << back.rts;
        return false;
    }
    return true;
}

// =======================================================================================
// SessionWidget over SIM:loopback
// =======================================================================================

void Tst_sessionwidget::loopbackConnect()
{
    auto session = newSession();
    QVERIFY(session);
    QCOMPARE(session->title(), QStringLiteral("New Session"));
    QVERIFY(!session->isConnected());
    QVERIFY(!session->terminal()->inputEnabled());

    QList<int> states;
    connect(session.get(), &SessionWidget::connectionStateChanged, this,
            [&states](State state) { states.append(static_cast<int>(state)); });
    QSignalSpy titleSpy(session.get(), &SessionWidget::titleChanged);

    session->setPortName(kLoopback);
    QCOMPARE(session->portName(), kLoopback);
    QCOMPARE(session->title(), kLoopback);
    QVERIFY(titleSpy.count() >= 1);
    QCOMPARE(titleSpy.last().at(0).toString(), kLoopback);
    QCOMPARE(session->connection()->settings().portName, kLoopback);

    auto* button = child<QPushButton>(session->connectionBar(), "connectButton");
    QVERIFY(button);
    QCOMPARE(button->text(), QStringLiteral("Connect"));

    QVERIFY(session->connectPort());
    QVERIFY(session->isConnected());
    QCOMPARE(session->connection()->state(), State::Connected);
    QCOMPARE(states, QList<int>{static_cast<int>(State::Connected)});
    QCOMPARE(button->text(), QStringLiteral("Disconnect"));
    QVERIFY(session->terminal()->inputEnabled());
    QCOMPARE(session->title(), kLoopback);
    QCOMPARE(AppSettings::instance().lastPortName(), kLoopback);

    // Connecting again is a no-op that reports success.
    QVERIFY(session->connectPort());
    QCOMPARE(states.size(), 1);
}

void Tst_sessionwidget::loopbackTerminalTyping()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    TerminalWidget* terminal = session->terminal();

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });

    QTest::keyClicks(terminal, QStringLiteral("hello"));
    QCOMPARE(sent, QByteArrayLiteral("hello"));
    QTRY_VERIFY_WITH_TIMEOUT(visibleText(terminal).contains(QStringLiteral("hello")), kSimTimeoutMs);
    QCOMPARE(firstRow(terminal).trimmed(), QStringLiteral("hello"));

    // Enter sends the configured line ending (CR by default); the echo returns the cursor.
    QTest::keyClick(terminal, Qt::Key_Return);
    QCOMPARE(sent, QByteArrayLiteral("hello\r"));
    QTRY_COMPARE_WITH_TIMEOUT(session->connection()->bytesReceived(), quint64(6), kSimTimeoutMs);
    QCOMPARE(terminal->screen()->cursor().col, 0);
}

void Tst_sessionwidget::loopbackCommandInputSend()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    CommandInput* input = session->commandInput();

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });
    QSignalSpy requestSpy(input, &CommandInput::sendRequested);

    input->setText(QStringLiteral("hello"));
    input->send();
    QCOMPARE(requestSpy.count(), 1);
    QCOMPARE(requestSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("hello\r"));
    QCOMPARE(requestSpy.at(0).at(1).toString(), QStringLiteral("hello"));
    QCOMPARE(sent, QByteArrayLiteral("hello\r"));
    QVERIFY(input->text().isEmpty());
    QCOMPARE(m_history.entries(), QStringList{QStringLiteral("hello")});
    QTRY_VERIFY_WITH_TIMEOUT(visibleText(session->terminal()).contains(QStringLiteral("hello")), kSimTimeoutMs);

    // The Send button takes the same path.
    auto* sendButton = child<QPushButton>(input, "sendButton");
    QVERIFY(sendButton);
    QVERIFY(sendButton->isEnabled());
    input->setText(QStringLiteral("world"));
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QCOMPARE(sent, QByteArrayLiteral("hello\rworld\r"));
}

void Tst_sessionwidget::loopbackHexModeRoundTrip()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    CommandInput* input = session->commandInput();

    QByteArray sent;
    QByteArray received;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });

    input->setHexMode(true);
    QVERIFY(input->hexMode());
    input->setText(QStringLiteral("41 42 0D"));
    input->send();
    QCOMPARE(sent, QByteArrayLiteral("AB\r"));
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("AB\r"), kSimTimeoutMs);

    const QString hex = session->hexView()->toPlainText();
    QVERIFY2(hex.contains(QStringLiteral("TX 3 bytes")), qPrintable(hex));
    QVERIFY2(hex.contains(QStringLiteral("RX 3 bytes")), qPrintable(hex));
    QCOMPARE(hex.count(QStringLiteral("41 42 0D")), qsizetype(2));
    QVERIFY(hex.indexOf(QStringLiteral("TX 3 bytes")) < hex.indexOf(QStringLiteral("RX 3 bytes")));
    QCOMPARE(firstRow(session->terminal()).trimmed(), QStringLiteral("AB"));
}

void Tst_sessionwidget::loopbackEscapeMode()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    CommandInput* input = session->commandInput();

    QByteArray sent;
    QByteArray received;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });

    input->setEscapeMode(true);
    QVERIFY(input->escapeMode());
    input->setText(QStringLiteral("a\\tb"));
    input->send();
    QCOMPARE(sent, QByteArrayLiteral("a\tb\r"));
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("a\tb\r"), kSimTimeoutMs);

    // The echoed tab moved the cursor to the next tab stop (column 8) before 'b'.
    const QString row = firstRow(session->terminal());
    QVERIFY2(row.startsWith(QLatin1Char('a')), qPrintable(row));
    QCOMPARE(row.indexOf(QLatin1Char('b')), qsizetype(8));
}

void Tst_sessionwidget::loopbackEscapeModeGbk()
{
    // In a non-UTF-8 session, Esc mode transcodes the text runs but keeps every \xHH byte exact.
    if (!QStringEncoder("GBK").isValid()) {
        QSKIP("This Qt build has no GBK codec (built without ICU)");
    }
    AppSettings::instance().setEncoding(QStringLiteral("GBK"));
    auto session = newSession();
    QVERIFY(session);
    QCOMPARE(session->terminal()->encoding(), QStringLiteral("GBK"));
    QVERIFY(connectTo(session.get(), kLoopback));
    CommandInput* input = session->commandInput();

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });

    input->setEscapeMode(true);
    input->setLineEnding(LineEnding::Mode::CR);
    input->setText(QStringLiteral("\\xb0\\xa1"));   // GBK for U+554A, typed byte by byte
    input->send();
    QCOMPARE(sent, QByteArray("\xb0\xa1\r", 3));

    // A literal CJK character is transcoded to its GBK bytes; the \xff next to it stays 0xFF.
    sent.clear();
    input->setText(QString::fromUtf8("\xe5\x95\x8a") + QStringLiteral("\\xff"));   // U+554A + "\xff"
    input->send();
    QCOMPARE(sent, QByteArray("\xb0\xa1\xff\r", 4));

    // Quick commands with escapes follow the same rule (an MCU token, no line ending).
    sent.clear();
    QuickCommand token = makeCommand(QStringLiteral("token"), QStringLiteral("\\xff\\x55"), QStringLiteral("MCU"));
    token.escapes = true;
    token.lineEnding = LineEnding::Mode::None;
    session->sendQuickCommand(token);
    QCOMPARE(sent, QByteArray("\xff\x55", 2));

    // Plain (non-Esc) text is still transcoded as a whole.
    sent.clear();
    input->setEscapeMode(false);
    input->setText(QString::fromUtf8("\xe5\x95\x8a"));
    input->send();
    QCOMPARE(sent, QByteArray("\xb0\xa1\r", 3));

    AppSettings::instance().setEncoding(QStringLiteral("UTF-8"));
}

void Tst_sessionwidget::loopbackEscapeModeLatin1()
{
    // Same contract with a codec every Qt build has (no ICU needed): the text run is transcoded
    // to Latin-1, the \xHH bytes stay exact, even when they would be invalid UTF-8.
    AppSettings::instance().setEncoding(QStringLiteral("ISO-8859-1"));
    auto session = newSession();
    QVERIFY(session);
    QCOMPARE(session->terminal()->encoding(), QStringLiteral("ISO-8859-1"));
    QVERIFY(connectTo(session.get(), kLoopback));
    CommandInput* input = session->commandInput();

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });

    input->setEscapeMode(true);
    input->setLineEnding(LineEnding::Mode::CR);
    input->setText(QString::fromUtf8("caf\xc3\xa9") + QStringLiteral("\\xff\\x80") + QString::fromUtf8("\xc3\xa9"));
    input->send();
    QCOMPARE(sent, QByteArray("caf\xe9\xff\x80\xe9\r", 8));

    // The \u escape is text too and follows the encoding; the quick command path agrees.
    sent.clear();
    QuickCommand qc = makeCommand(QStringLiteral("u"), QStringLiteral("\\u00e9\\xfe"), QStringLiteral("Test"));
    qc.escapes = true;
    qc.lineEnding = LineEnding::Mode::CRLF;
    session->sendQuickCommand(qc);
    QCOMPARE(sent, QByteArray("\xe9\xfe\r\n", 4));

    // HEX mode is unaffected by the encoding.
    sent.clear();
    input->setEscapeMode(false);
    input->setHexMode(true);
    input->setText(QStringLiteral("C3 A9"));
    input->send();
    QCOMPARE(sent, QByteArray("\xc3\xa9", 2));

    AppSettings::instance().setEncoding(QStringLiteral("UTF-8"));
}

void Tst_sessionwidget::loopbackHistoryNavigation()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    CommandInput* input = session->commandInput();
    auto* edit = child<QLineEdit>(input, "commandEdit");
    QVERIFY(edit);

    input->setText(QStringLiteral("first"));
    input->send();
    input->setText(QStringLiteral("second"));
    input->send();
    QCOMPARE(m_history.entries(), (QStringList{QStringLiteral("first"), QStringLiteral("second")}));

    QTest::keyClicks(edit, QStringLiteral("dra"));
    QCOMPARE(input->text(), QStringLiteral("dra"));
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input->text(), QStringLiteral("second"));
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input->text(), QStringLiteral("first"));
    QTest::keyClick(edit, Qt::Key_Up); // stays at the oldest entry
    QCOMPARE(input->text(), QStringLiteral("first"));
    QTest::keyClick(edit, Qt::Key_Down);
    QCOMPARE(input->text(), QStringLiteral("second"));
    QTest::keyClick(edit, Qt::Key_Down); // back below the newest entry: the draft returns
    QCOMPARE(input->text(), QStringLiteral("dra"));
    QVERIFY(!m_history.isNavigating());

    // Sending the recalled draft appends it to the shared history.
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(m_history.entries().last(), QStringLiteral("dra"));
    QVERIFY(input->text().isEmpty());
}

void Tst_sessionwidget::loopbackQuickCommand()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    const QList<QuickCommand> defaults = QuickCommandStore::defaults();
    QCOMPARE(defaults.first().command, QStringLiteral("uname -a"));
    session->sendQuickCommand(defaults.first());
    QCOMPARE(sent, QByteArrayLiteral("uname -a\r"));
    QTRY_VERIFY_WITH_TIMEOUT(visibleText(session->terminal()).contains(QStringLiteral("uname -a")), kSimTimeoutMs);

    // A hex control command sends the raw byte without a line ending.
    QuickCommand ctrlC;
    for (const QuickCommand& qc : defaults) {
        if (qc.name == QStringLiteral("Ctrl+C")) {
            ctrlC = qc;
        }
    }
    QVERIFY(ctrlC.hex);
    session->sendQuickCommand(ctrlC);
    QCOMPARE(sent, QByteArrayLiteral("uname -a\r\x03"));

    // An invalid payload is reported in the status bar and nothing is sent.
    statusSpy.clear();
    QuickCommand bad = makeCommand(QStringLiteral("bad"), QStringLiteral("4G"), QStringLiteral("Test"));
    bad.hex = true;
    session->sendQuickCommand(bad);
    QCOMPARE(sent, QByteArrayLiteral("uname -a\r\x03"));
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY2(statusSpy.at(0).at(0).toString().contains(QStringLiteral("bad")),
             qPrintable(statusSpy.at(0).at(0).toString()));
}

void Tst_sessionwidget::loopbackQuickCommandBarClick()
{
    auto session = newSession();
    QVERIFY(session);
    QuickCommandBar* bar = session->quickCommandBar();
    QCOMPARE(bar->store(), m_store);

    // Disabled until the session is connected.
    QList<QToolButton*> buttons = commandButtons(bar);
    QCOMPARE(buttons.size(), QuickCommandStore::defaults().size());
    QVERIFY(!buttons.first()->isEnabled());

    QVERIFY(connectTo(session.get(), kLoopback));
    QVERIFY(buttons.first()->isEnabled());
    QVERIFY(buttons.first()->width() > 0);

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });
    QTest::mouseClick(buttons.first(), Qt::LeftButton);
    QCOMPARE(sent, QByteArrayLiteral("uname -a\r"));
    QTRY_VERIFY_WITH_TIMEOUT(visibleText(session->terminal()).contains(QStringLiteral("uname -a")), kSimTimeoutMs);

    // The gear asks MainWindow to open the editor.
    QSignalSpy editSpy(session.get(), &SessionWidget::quickCommandsEditRequested);
    auto* gear = child<QToolButton>(bar, "editButton");
    QVERIFY(gear);
    QTest::mouseClick(gear, Qt::LeftButton);
    QCOMPARE(editSpy.count(), 1);

    session->disconnectPort();
    QVERIFY(!buttons.first()->isEnabled());
}

void Tst_sessionwidget::loopbackLogging()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));

    QByteArray received;
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });
    QSignalSpy loggingSpy(session.get(), &SessionWidget::loggingChanged);

    const QString logPath = tempPath(QStringLiteral("session.log"));
    QVERIFY(!session->isLogging());
    session->startLoggingTo(logPath);
    QVERIFY(session->isLogging());
    QCOMPARE(session->logFilePath(), logPath);
    QCOMPARE(session->logger()->format(), SessionLogger::Format::Text);
    QCOMPARE(loggingSpy.count(), 1);
    QCOMPARE(loggingSpy.at(0).at(0).toBool(), true);
    QCOMPARE(loggingSpy.at(0).at(1).toString(), logPath);

    // Starting the same file again is a no-op; a different file switches the log.
    session->startLoggingTo(logPath);
    QCOMPARE(loggingSpy.count(), 1);

    session->commandInput()->setText(QStringLiteral("hello"));
    session->commandInput()->send();
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("hello\r"), kSimTimeoutMs);

    session->stopLogging();
    QVERIFY(!session->isLogging());
    QCOMPARE(loggingSpy.count(), 2);
    QCOMPARE(loggingSpy.at(1).at(0).toBool(), false);
    QCOMPARE(loggingSpy.at(1).at(1).toString(), logPath);
    session->stopLogging(); // idempotent
    QCOMPARE(loggingSpy.count(), 2);

    const QString text = QString::fromUtf8(readFile(logPath));
    const QStringList lines = text.split(QLatin1Char('\n'));
    QVERIFY2(lines.size() >= 3, qPrintable(text));
    // Header written exactly once: "# BuildAI Serial Utility log - <port> <settings> - started <time>".
    QVERIFY2(
        lines.at(0).startsWith(QStringLiteral("# BuildAI Serial Utility log - SIM:loopback 115200 8N1 - started ")),
        qPrintable(lines.at(0)));
    QCOMPARE(lines.at(0).count(QStringLiteral("BuildAI Serial Utility log")), qsizetype(1));
    QCOMPARE(lines.at(0).count(QStringLiteral("started")), qsizetype(1));
    // TX on its own timestamped line with escaped bytes, RX prefixed with a timestamp.
    const QRegularExpression txLine(
        QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] TX> hello\\\\r$"));
    QVERIFY2(txLine.match(lines.at(1)).hasMatch(), qPrintable(lines.at(1)));
    const QRegularExpression rxLine(
        QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\] hello\r"));
    QVERIFY2(rxLine.match(lines.at(2)).hasMatch(), qPrintable(lines.at(2)));
}

void Tst_sessionwidget::loopbackLoggingRawFormat()
{
    AppSettings::instance().setLogFormat(QStringLiteral("raw"));
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));

    QByteArray received;
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });

    const QString logPath = tempPath(QStringLiteral("session-raw.log"));
    session->startLoggingTo(logPath);
    QVERIFY(session->isLogging());
    QCOMPARE(session->logger()->format(), SessionLogger::Format::Raw);

    session->sendBytes(QByteArrayLiteral("raw \x1b[32mbytes\x1b[0m\r\n"));
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("raw \x1b[32mbytes\x1b[0m\r\n"), kSimTimeoutMs);
    session->stopLogging();

    // Raw: received bytes verbatim, no header, TX never included.
    QCOMPARE(readFile(logPath), QByteArrayLiteral("raw \x1b[32mbytes\x1b[0m\r\n"));
}

void Tst_sessionwidget::loopbackAutoLogOnConnect()
{
    AppSettings::instance().setAutoLog(true);
    auto session = newSession();
    QVERIFY(session);
    QSignalSpy loggingSpy(session.get(), &SessionWidget::loggingChanged);

    QVERIFY(connectTo(session.get(), kLoopback));
    QVERIFY(session->isLogging());
    QCOMPARE(loggingSpy.count(), 1);
    const QString logPath = session->logFilePath();
    QVERIFY2(logPath.startsWith(AppSettings::instance().logDirectory()), qPrintable(logPath));
    QVERIFY2(QFileInfo(logPath).fileName().startsWith(QStringLiteral("SIM_loopback_")), qPrintable(logPath));
    QVERIFY(QFile::exists(logPath));

    QByteArray received;
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });
    session->sendBytes(QByteArrayLiteral("auto\r"));
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("auto\r"), kSimTimeoutMs);

    session->toggleLogging(); // active -> stop
    QVERIFY(!session->isLogging());
    const QString text = QString::fromUtf8(readFile(logPath));
    QVERIFY2(text.startsWith(QStringLiteral("# BuildAI Serial Utility log - SIM:loopback")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("TX> auto\\r")));
}

void Tst_sessionwidget::autoLogSwitchesFileOnPortChange()
{
    AppSettings::instance().setAutoLog(true);
    auto session = newSession();
    QVERIFY(session);
    QSignalSpy loggingSpy(session.get(), &SessionWidget::loggingChanged);

    QVERIFY(connectTo(session.get(), kLoopback));
    QVERIFY(session->isLogging());
    const QString pathA = session->logFilePath();
    QVERIFY2(QFileInfo(pathA).fileName().startsWith(QStringLiteral("SIM_loopback_")), qPrintable(pathA));

    // Disconnecting alone keeps the auto-log open and unchanged (same port may reconnect).
    session->disconnectPort();
    QVERIFY(session->isLogging());
    QCOMPARE(session->logFilePath(), pathA);
    QCOMPARE(loggingSpy.count(), 1);

    // Connecting to a different port closes the loopback log and starts one named after the new port.
    QVERIFY(connectTo(session.get(), kLinux));
    QVERIFY(session->isLogging());
    const QString pathB = session->logFilePath();
    QVERIFY2(pathB != pathA, qPrintable(pathB));
    QVERIFY2(QFileInfo(pathB).fileName().startsWith(QStringLiteral("SIM_linux_")), qPrintable(pathB));
    QCOMPARE(loggingSpy.count(), 3);
    QCOMPARE(loggingSpy.at(0).at(0).toBool(), true);
    QCOMPARE(loggingSpy.at(0).at(1).toString(), pathA);
    QCOMPARE(loggingSpy.at(1).at(0).toBool(), false);
    QCOMPARE(loggingSpy.at(1).at(1).toString(), pathA);
    QCOMPARE(loggingSpy.at(2).at(0).toBool(), true);
    QCOMPARE(loggingSpy.at(2).at(1).toString(), pathB);

    // Each file's header names its own port.
    const QString headerA = QString::fromUtf8(readFile(pathA)).section(QLatin1Char('\n'), 0, 0);
    QVERIFY2(headerA.startsWith(QStringLiteral("# BuildAI Serial Utility log - SIM:loopback ")), qPrintable(headerA));
    const QString headerB = QString::fromUtf8(readFile(pathB)).section(QLatin1Char('\n'), 0, 0);
    QVERIFY2(headerB.startsWith(QStringLiteral("# BuildAI Serial Utility log - SIM:linux ")), qPrintable(headerB));

    session->disconnectPort();
}

void Tst_sessionwidget::manualLogKeptAcrossPortChange()
{
    AppSettings::instance().setAutoLog(true);
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    QVERIFY(session->isLogging());

    // The user replaces the auto-log by a file of their own choice.
    session->stopLogging();
    QVERIFY(!session->isLogging());
    const QString manual = tempPath(QStringLiteral("manual.log"));
    session->startLoggingTo(manual);
    QVERIFY(session->isLogging());
    QCOMPARE(session->logFilePath(), manual);

    // Switching ports must not throw the user's file away in favour of an auto-log.
    session->disconnectPort();
    QVERIFY(connectTo(session.get(), kLinux));
    QVERIFY(session->isLogging());
    QCOMPARE(session->logFilePath(), manual);

    session->disconnectPort();
}

void Tst_sessionwidget::loopbackViewModeSwitch()
{
    auto session = newSession();
    QVERIFY(session);
    auto* stack = child<QStackedWidget>(session.get(), "viewStack");
    QVERIFY(stack);
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::Terminal);
    QCOMPARE(stack->currentWidget(), static_cast<QWidget*>(session->terminal()));

    QList<int> modes;
    connect(session.get(), &SessionWidget::viewModeChanged, this,
            [&modes](SessionWidget::ViewMode mode) { modes.append(static_cast<int>(mode)); });

    session->setViewMode(SessionWidget::ViewMode::HexDump);
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::HexDump);
    QCOMPARE(stack->currentWidget(), static_cast<QWidget*>(session->hexView()));
    QCOMPARE(modes, QList<int>{static_cast<int>(SessionWidget::ViewMode::HexDump)});
    QCOMPARE(session->focusProxy(), static_cast<QWidget*>(session->hexView()));

    session->setViewMode(SessionWidget::ViewMode::HexDump); // no change, no signal
    QCOMPARE(modes.size(), 1);

    // Traffic keeps flowing into both views regardless of which one is shown.
    QVERIFY(connectTo(session.get(), kLoopback));
    session->sendBytes(QByteArrayLiteral("hex view\r"));
    QTRY_VERIFY_WITH_TIMEOUT(session->hexView()->toPlainText().contains(QStringLiteral("RX 9 bytes")), kSimTimeoutMs);
    QVERIFY(visibleText(session->terminal()).contains(QStringLiteral("hex view")));

    session->setViewMode(SessionWidget::ViewMode::Terminal);
    QCOMPARE(stack->currentWidget(), static_cast<QWidget*>(session->terminal()));
    QCOMPARE(modes.size(), 2);
    QCOMPARE(modes.last(), static_cast<int>(SessionWidget::ViewMode::Terminal));
    QCOMPARE(session->focusProxy(), static_cast<QWidget*>(session->terminal()));
}

void Tst_sessionwidget::loopbackDisconnect()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    auto* button = child<QPushButton>(session->connectionBar(), "connectButton");
    QVERIFY(button);

    QList<int> states;
    connect(session.get(), &SessionWidget::connectionStateChanged, this,
            [&states](State state) { states.append(static_cast<int>(state)); });
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    session->disconnectPort();
    QVERIFY(!session->isConnected());
    QCOMPARE(session->connection()->state(), State::Disconnected);
    QCOMPARE(states, QList<int>{static_cast<int>(State::Disconnected)});
    QVERIFY(!session->terminal()->inputEnabled());
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QCOMPARE(session->title(), kLoopback); // the port stays selected
    QVERIFY(statusSpy.count() >= 1);
    QVERIFY2(statusSpy.last().at(0).toString().contains(QStringLiteral("Disconnected from SIM:loopback")),
             qPrintable(statusSpy.last().at(0).toString()));

    // Every TX path reports "Not connected" and sends nothing.
    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });
    statusSpy.clear();
    session->sendBytes(QByteArrayLiteral("x"));
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QStringLiteral("Not connected"));
    QVERIFY(sent.isEmpty());

    QSignalSpy requestSpy(session->commandInput(), &CommandInput::sendRequested);
    session->commandInput()->setText(QStringLiteral("hello"));
    session->commandInput()->send();
    QCOMPARE(requestSpy.count(), 0);
    QCOMPARE(session->commandInput()->text(), QStringLiteral("hello")); // kept for later
    QVERIFY(sent.isEmpty());

    QTest::keyClicks(session->terminal(), QStringLiteral("abc"));
    QVERIFY(sent.isEmpty());

    // Disconnecting twice is harmless and silent.
    statusSpy.clear();
    session->disconnectPort();
    QCOMPARE(statusSpy.count(), 0);
    QCOMPARE(states.size(), 1);
}

void Tst_sessionwidget::loopbackSendFileDialog()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));

    const QString filePath = tempPath(QStringLiteral("send.txt"));
    QVERIFY(writeFile(filePath, QByteArrayLiteral("line one\nline two\nline three\n")));

    QByteArray received;
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });

    QVERIFY(!session->findChild<SendFileDialog*>());
    session->sendFile(filePath);
    auto* dialog = session->findChild<SendFileDialog*>();
    QVERIFY(dialog);
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    QVERIFY(dialog->isVisible());
    QVERIFY(!dialog->isModal());
    QCOMPARE(dialog->filePath(), filePath);
    QVERIFY(!dialog->isSending());

    // Send with CRLF so the echoed lines land on separate terminal rows, and without pacing delays.
    auto* endingCombo = child<QComboBox>(dialog, "lineEndingCombo");
    QVERIFY(endingCombo);
    endingCombo->setCurrentIndex(endingCombo->findData(static_cast<int>(LineEnding::Mode::CRLF)));
    auto* lineDelay = child<QSpinBox>(dialog, "lineDelaySpin");
    QVERIFY(lineDelay);
    lineDelay->setValue(10);
    QVERIFY(dialog->options().lineEnding == LineEnding::Mode::CRLF);
    QCOMPARE(dialog->options().mode, FileSender::Mode::TextLines);

    auto* startButton = child<QPushButton>(dialog, "startButton");
    QVERIFY(startButton);
    QVERIFY(startButton->isEnabled());
    QSignalSpy startedSpy(dialog, &SendFileDialog::sendingStarted);
    QSignalSpy finishedSpy(dialog, &SendFileDialog::sendingFinished);
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    QTest::mouseClick(startButton, Qt::LeftButton);
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(statusSpy.count() >= 1);
    QVERIFY(statusSpy.at(0).at(0).toString().contains(QStringLiteral("send.txt")));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, kSimTimeoutMs);
    QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
    QVERIFY(!dialog->isSending());
    QVERIFY(startButton->isEnabled());

    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("line one\r\nline two\r\nline three\r\n"), kSimTimeoutMs);
    const QString text = visibleText(session->terminal());
    QVERIFY2(text.contains(QStringLiteral("line one\nline two\nline three")), qPrintable(text));

    // A second sendFile() reuses the same modeless dialog.
    session->sendFile();
    QCOMPARE(session->findChildren<SendFileDialog*>().size(), qsizetype(1));
    QCOMPARE(dialog->filePath(), filePath);

    // Start is disabled while the session is disconnected.
    session->disconnectPort();
    QVERIFY(!startButton->isEnabled());
    dialog->close();
}

void Tst_sessionwidget::loopbackSyncTerminalSize()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    TerminalWidget* terminal = session->terminal();
    QVERIFY(terminal->columns() > 0);
    QVERIFY(terminal->visibleRows() > 0);

    QByteArray sent;
    connect(session->connection(), &SerialConnection::dataSent, this,
            [&sent](const QByteArray& bytes) { sent += bytes; });

    session->syncTerminalSize();
    const QByteArray expected =
        QStringLiteral("stty cols %1 rows %2\r").arg(terminal->columns()).arg(terminal->visibleRows()).toLatin1();
    QCOMPARE(sent, expected);
    QVERIFY(sent.startsWith(QByteArrayLiteral("stty cols ")));
    QTRY_VERIFY_WITH_TIMEOUT(visibleText(terminal).contains(QStringLiteral("stty cols")), kSimTimeoutMs);

    // The terminal's context-menu request takes the same path.
    emit terminal->syncSizeRequested();
    QCOMPARE(sent, expected + expected);

    // The context-menu "Find..." request is forwarded for MainWindow to show the prompt.
    QSignalSpy findSpy(session.get(), &SessionWidget::findRequested);
    emit terminal->findRequested();
    QCOMPARE(findSpy.count(), 1);
}

void Tst_sessionwidget::loopbackLiveBaudChange()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    ConnectionBar* bar = session->connectionBar();
    QCOMPARE(session->connection()->settings().baudRate, 115200);

    QList<SerialSettings> changes;
    connect(bar, &ConnectionBar::settingsChanged, this,
            [&changes](const SerialSettings& settings) { changes.append(settings); });

    auto* baudCombo = child<QComboBox>(bar, "baudCombo");
    QVERIFY(baudCombo);
    QVERIFY(baudCombo->isEnabled()); // parameter combos stay live while connected
    const int index = baudCombo->findData(1500000);
    QVERIFY(index >= 0);
    baudCombo->setCurrentIndex(index);
    QCOMPARE(changes.size(), qsizetype(1));
    QCOMPARE(changes.last().baudRate, 1500000);
    QCOMPARE(changes.last().portName, kLoopback);
    QCOMPARE(session->connection()->settings().baudRate, 1500000);
    QCOMPARE(bar->settings().baudRate, 1500000);
    QVERIFY(session->isConnected());
    // After onBarSettingsChanged the bar shows exactly what the connection accepted. A SIM: port
    // accepts everything, so both sides agree; the driver-rejection branch (combo snaps back to
    // the value the port really uses) cannot be provoked without hardware and is review-only.
    QCOMPARE(bar->settings(), session->connection()->settings());

    auto* parityCombo = child<QComboBox>(bar, "parityCombo");
    QVERIFY(parityCombo);
    parityCombo->setCurrentIndex(parityCombo->findData(static_cast<int>(QSerialPort::EvenParity)));
    QCOMPARE(changes.size(), qsizetype(2));
    QCOMPARE(session->connection()->settings().parity, QSerialPort::EvenParity);
    QCOMPARE(session->connection()->settings().summary(), QStringLiteral("1500000 8E1"));
    QCOMPARE(bar->settings(), session->connection()->settings());

    // The device still echoes after the live change.
    QByteArray received;
    connect(session->connection(), &SerialConnection::dataReceived, this,
            [&received](const QByteArray& bytes) { received += bytes; });
    session->sendBytes(QByteArrayLiteral("fast\r"));
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArrayLiteral("fast\r"), kSimTimeoutMs);
}

void Tst_sessionwidget::loopbackCounters()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));

    quint64 rx = 0;
    quint64 tx = 0;
    int emissions = 0;
    connect(session.get(), &SessionWidget::countersChanged, this, [&](quint64 r, quint64 t) {
        rx = r;
        tx = t;
        ++emissions;
    });

    session->sendBytes(QByteArrayLiteral("hello\r"));
    QCOMPARE(tx, quint64(6));
    QTRY_COMPARE_WITH_TIMEOUT(rx, quint64(6), kSimTimeoutMs);
    QVERIFY(emissions >= 2);
    QCOMPARE(session->connection()->bytesSent(), quint64(6));
    QCOMPARE(session->connection()->bytesReceived(), quint64(6));

    session->sendBytes(QByteArrayLiteral("ab"));
    QCOMPARE(tx, quint64(8));
    QTRY_COMPARE_WITH_TIMEOUT(rx, quint64(8), kSimTimeoutMs);

    session->connection()->resetCounters();
    QCOMPARE(rx, quint64(0));
    QCOMPARE(tx, quint64(0));
}

void Tst_sessionwidget::loopbackConnectWithoutPort()
{
    auto session = newSession();
    QVERIFY(session);
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);
    QList<int> states;
    connect(session.get(), &SessionWidget::connectionStateChanged, this,
            [&states](State state) { states.append(static_cast<int>(state)); });

    QVERIFY(!session->connectPort());
    QVERIFY(!session->isConnected());
    QVERIFY(states.isEmpty());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QStringLiteral("Select a serial port first"));

    // An unknown simulated device fails with the connection's error message.
    session->setPortName(QStringLiteral("SIM:nothing"));
    statusSpy.clear();
    QVERIFY(!session->connectPort());
    QVERIFY(!session->isConnected());
    QVERIFY(states.isEmpty());
    QVERIFY(statusSpy.count() >= 1);
    QVERIFY2(statusSpy.at(0).at(0).toString().contains(QStringLiteral("SIM:nothing")),
             qPrintable(statusSpy.at(0).at(0).toString()));
    QCOMPARE(session->title(), QStringLiteral("SIM:nothing"));
}

void Tst_sessionwidget::loopbackToggleConnection()
{
    auto session = newSession();
    QVERIFY(session);
    session->setPortName(kLoopback);
    auto* button = child<QPushButton>(session->connectionBar(), "connectButton");
    QVERIFY(button);

    session->toggleConnection();
    QVERIFY(session->isConnected());
    session->toggleConnection();
    QVERIFY(!session->isConnected());

    // The bar's Connect/Disconnect button drives the same slots.
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(session->isConnected());
    QCOMPARE(button->text(), QStringLiteral("Disconnect"));
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(!session->isConnected());
    QCOMPARE(button->text(), QStringLiteral("Connect"));
}

void Tst_sessionwidget::loopbackClearTerminal()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    TerminalWidget* terminal = session->terminal();

    session->sendBytes(QByteArrayLiteral("keep me\r\n"));
    QTRY_VERIFY_WITH_TIMEOUT(visibleText(terminal).contains(QStringLiteral("keep me")), kSimTimeoutMs);
    QVERIFY(!session->hexView()->toPlainText().isEmpty());

    session->clearTerminal();
    QVERIFY(visibleText(terminal).trimmed().isEmpty());
    QVERIFY(session->hexView()->toPlainText().isEmpty());
    // The cleared screen went into the scrollback, nothing is lost.
    QVERIFY(allText(terminal).contains(QStringLiteral("keep me")));

    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);
    session->resetTerminal();
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QStringLiteral("Terminal reset"));
    QVERIFY(!allText(terminal).contains(QStringLiteral("keep me"))); // RIS drops the scrollback too
}

void Tst_sessionwidget::loopbackSendBreak()
{
    auto session = newSession();
    QVERIFY(session);
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    session->sendBreak();
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QStringLiteral("Not connected"));

    QVERIFY(connectTo(session.get(), kLoopback));
    statusSpy.clear();
    session->sendBreak();
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QStringLiteral("BREAK sent"));

    // The bar's Break button is enabled while connected and takes the same path.
    auto* breakButton = child<QToolButton>(session->connectionBar(), "breakButton");
    QVERIFY(breakButton);
    QVERIFY(breakButton->isEnabled());
    statusSpy.clear();
    QTest::mouseClick(breakButton, Qt::LeftButton);
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(), QStringLiteral("BREAK sent"));
}

// =======================================================================================
// SessionWidget over SIM:linux
// =======================================================================================

void Tst_sessionwidget::linuxLoginUnameReboot()
{
    auto session = newSession();
    QVERIFY(session);
    TerminalWidget* terminal = session->terminal();
    auto* button = child<QPushButton>(session->connectionBar(), "connectButton");
    QVERIFY(button);

    QList<int> states;
    connect(session.get(), &SessionWidget::connectionStateChanged, this,
            [&states](State state) { states.append(static_cast<int>(state)); });
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    QVERIFY(connectTo(session.get(), kLinux));
    QCOMPARE(session->title(), kLinux);

    // Boot log, then the login prompt.
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).contains(QStringLiteral("rv1106 login:")), kBootTimeoutMs);
    QTest::keyClicks(terminal, QStringLiteral("root"));
    QTest::keyClick(terminal, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).contains(QStringLiteral("Password:")), kSimTimeoutMs);
    QTest::keyClick(terminal, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).contains(QStringLiteral("[root@rv1106:~]#")), kSimTimeoutMs);

    // A shell command through the line-mode input.
    session->commandInput()->setText(QStringLiteral("uname -a"));
    session->commandInput()->send();
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).contains(QStringLiteral("Linux rv1106 5.10.160")), kSimTimeoutMs);
    QVERIFY(allText(terminal).contains(QStringLiteral("GNU/Linux")));

    // Reboot: the port vanishes, the session reconnects automatically (200 ms poll).
    states.clear();
    statusSpy.clear();
    session->commandInput()->setText(QStringLiteral("reboot"));
    session->commandInput()->send();
    QTRY_VERIFY_WITH_TIMEOUT(states.contains(static_cast<int>(State::Reconnecting)), kSimTimeoutMs);
    QCOMPARE(states, QList<int>{static_cast<int>(State::Reconnecting)});
    QVERIFY(!session->isConnected());
    QVERIFY(!terminal->inputEnabled());
    QCOMPARE(button->text(), QStringLiteral("Reconnecting..."));
    QVERIFY(allText(terminal).contains(QStringLiteral("The system is going down for reboot NOW!")));

    QTRY_VERIFY_WITH_TIMEOUT(states.contains(static_cast<int>(State::Connected)), kSimTimeoutMs);
    QCOMPARE(states, (QList<int>{static_cast<int>(State::Reconnecting), static_cast<int>(State::Connected)}));
    QVERIFY(session->isConnected());
    QVERIFY(terminal->inputEnabled());
    QCOMPARE(button->text(), QStringLiteral("Disconnect"));

    const QString text = allText(terminal);
    QVERIFY2(text.contains(QStringLiteral("--- port SIM:linux disappeared")), qPrintable(text.right(600)));
    QVERIFY2(text.contains(QStringLiteral("--- reconnected")), qPrintable(text.right(600)));
    QVERIFY(text.indexOf(QStringLiteral("--- port SIM:linux disappeared")) <
            text.indexOf(QStringLiteral("--- reconnected")));

    bool sawDisappeared = false;
    bool sawReconnected = false;
    for (const QList<QVariant>& args : statusSpy) {
        const QString message = args.at(0).toString();
        sawDisappeared = sawDisappeared || message.contains(QStringLiteral("SIM:linux disappeared"));
        sawReconnected = sawReconnected || message.contains(QStringLiteral("Reconnected to SIM:linux"));
    }
    QVERIFY(sawDisappeared);
    QVERIFY(sawReconnected);

    // The board boots again after the reconnect.
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).count(QStringLiteral("rv1106 login:")) >= 2, kBootTimeoutMs);
}

// =======================================================================================
// Log replay
// =======================================================================================

void Tst_sessionwidget::replayRawFile()
{
    auto session = newSession();
    QVERIFY(session);
    TerminalWidget* terminal = session->terminal();
    const QString path = tempPath(QStringLiteral("capture.log"));
    QVERIFY(writeFile(path, QByteArrayLiteral("boot line one\r\nboot line two\r\n\x1b[32m[ OK ]\x1b[0m done\r\n")));

    QList<bool> replayStates;
    connect(session.get(), &SessionWidget::replayStateChanged, this,
            [&replayStates](bool active) { replayStates.append(active); });
    QSignalSpy titleSpy(session.get(), &SessionWidget::titleChanged);
    QVERIFY(!session->isReplaying());

    session->replayLogFile(path, 0);
    QVERIFY(session->isReplaying());
    QCOMPARE(replayStates, QList<bool>{true});
    QVERIFY2(session->title().contains(QStringLiteral("capture.log")), qPrintable(session->title()));
    QVERIFY(titleSpy.count() >= 1);
    QVERIFY(titleSpy.last().at(0).toString().contains(QStringLiteral("capture.log")));

    QTRY_COMPARE_WITH_TIMEOUT(replayStates.size(), qsizetype(2), kSimTimeoutMs);
    QCOMPARE(replayStates.at(1), false);
    QVERIFY(!session->isReplaying());
    QCOMPARE(session->title(), QStringLiteral("New Session"));
    QCOMPARE(titleSpy.last().at(0).toString(), QStringLiteral("New Session"));

    const QString text = allText(terminal);
    QVERIFY2(text.contains(QStringLiteral("boot line one")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("boot line two")));
    QVERIFY(text.contains(QStringLiteral("[ OK ] done")));
    QVERIFY(text.contains(QStringLiteral("--- replaying")));
    QVERIFY(text.contains(QStringLiteral("replay of capture.log finished")));
    // Replayed bytes take the RX path into the hex view as well; the session stays disconnected.
    QVERIFY(session->hexView()->toPlainText().contains(QStringLiteral("RX")));
    QVERIFY(!session->isConnected());
    QVERIFY(!terminal->inputEnabled());
}

void Tst_sessionwidget::replayRefusedWhileConnected()
{
    auto session = newSession();
    QVERIFY(session);
    QVERIFY(connectTo(session.get(), kLoopback));
    const QString path = tempPath(QStringLiteral("refused.log"));
    QVERIFY(writeFile(path, QByteArrayLiteral("replayed line\r\n")));

    QList<bool> replayStates;
    connect(session.get(), &SessionWidget::replayStateChanged, this,
            [&replayStates](bool active) { replayStates.append(active); });
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    session->replayLogFile(path, 0);
    QVERIFY(!session->isReplaying());
    QVERIFY(replayStates.isEmpty());
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY2(statusSpy.at(0).at(0).toString().contains(kLoopback), qPrintable(statusSpy.at(0).at(0).toString()));
    QCOMPARE(session->title(), kLoopback);
    QTest::qWait(50);
    QVERIFY(!allText(session->terminal()).contains(QStringLiteral("replayed line")));
    QVERIFY(session->isConnected());

    // Once disconnected the same request runs.
    session->disconnectPort();
    session->replayLogFile(path, 0);
    QVERIFY(session->isReplaying());
    QTRY_VERIFY_WITH_TIMEOUT(!session->isReplaying(), kSimTimeoutMs);
    QCOMPARE(replayStates, (QList<bool>{true, false}));
    QVERIFY(allText(session->terminal()).contains(QStringLiteral("replayed line")));
}

void Tst_sessionwidget::replayStopMidway()
{
    auto session = newSession();
    QVERIFY(session);
    TerminalWidget* terminal = session->terminal();
    const QString path = tempPath(QStringLiteral("slow.log"));
    QByteArray payload(2000, 'x');
    payload += QByteArrayLiteral("\r\nEND\r\n");
    QVERIFY(writeFile(path, payload));

    QList<bool> replayStates;
    connect(session.get(), &SessionWidget::replayStateChanged, this,
            [&replayStates](bool active) { replayStates.append(active); });
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    session->replayLogFile(path, 100); // 100 bytes/s: the 2 KB file would take 20 s
    QVERIFY(session->isReplaying());
    QCOMPARE(session->title(), QStringLiteral("Replay: slow.log"));
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).contains(QStringLiteral("xx")), kSimTimeoutMs);
    QVERIFY(session->isReplaying());

    statusSpy.clear();
    session->stopReplay();
    QVERIFY(!session->isReplaying());
    QCOMPARE(replayStates, (QList<bool>{true, false}));
    QCOMPARE(session->title(), QStringLiteral("New Session"));
    QVERIFY(!allText(terminal).contains(QStringLiteral("END")));
    QVERIFY(allText(terminal).contains(QStringLiteral("replay of slow.log stopped")));
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(statusSpy.at(0).at(0).toString().contains(QStringLiteral("stopped")));

    // Nothing more arrives after the stop; stopping again is a no-op.
    const QString frozen = allText(terminal);
    QTest::qWait(50);
    QCOMPARE(allText(terminal), frozen);
    session->stopReplay();
    QCOMPARE(replayStates.size(), qsizetype(2));
}

void Tst_sessionwidget::connectDuringReplayStopsReplay()
{
    auto session = newSession();
    QVERIFY(session);
    TerminalWidget* terminal = session->terminal();
    const QString path = tempPath(QStringLiteral("slow.log"));
    QByteArray payload(2000, 'x');
    payload += QByteArrayLiteral("\r\nEND\r\n");
    QVERIFY(writeFile(path, payload));

    QList<bool> replayStates;
    connect(session.get(), &SessionWidget::replayStateChanged, this,
            [&replayStates](bool active) { replayStates.append(active); });
    QSignalSpy statusSpy(session.get(), &SessionWidget::statusMessage);

    session->replayLogFile(path, 100); // 100 bytes/s: the 2 KB file would take 20 s
    QVERIFY(session->isReplaying());
    QTRY_VERIFY_WITH_TIMEOUT(allText(terminal).contains(QStringLiteral("xx")), kSimTimeoutMs);
    QVERIFY(session->isReplaying());

    // Connecting while the replay streams stops the replay first, then opens the port.
    statusSpy.clear();
    session->setPortName(kLoopback);
    QVERIFY(session->connectPort());
    QVERIFY(!session->isReplaying());
    QVERIFY(session->isConnected());
    QCOMPARE(replayStates, (QList<bool>{true, false}));
    QCOMPARE(session->title(), kLoopback);
    QVERIFY2(allText(terminal).contains(QStringLiteral("replay of slow.log stopped")), qPrintable(allText(terminal)));
    bool sawConnected = false;
    for (const QList<QVariant>& args : statusSpy) {
        const QString message = args.at(0).toString();
        sawConnected =
            sawConnected || (message.contains(QStringLiteral("Connected to")) && message.contains(kLoopback));
    }
    QVERIFY(sawConnected);

    // No further replay chunks arrive once the port is open.
    QTest::qWait(150);
    QVERIFY(!allText(terminal).contains(QStringLiteral("END")));
    QVERIFY(session->isConnected());

    session->disconnectPort();
}

void Tst_sessionwidget::replayTimestampedCapture()
{
    auto session = newSession();
    QVERIFY(session);
    const QString path = tempPath(QStringLiteral("capture-text.log"));
    const QByteArray capture =
        QByteArrayLiteral("# BuildAI Serial Utility log - SIM:linux 115200 8N1 - started 2026-09-20 10:00:00.000\n"
                          "[2026-09-20 10:00:00.900] [root@rv1106:~]# \n"
                          "[2026-09-20 10:00:01.000] TX> uname -a\\r\n"
                          "[2026-09-20 10:00:01.050] uname -a\n"
                          "[2026-09-20 10:00:01.100] Linux rv1106 5.10.160 armv7l GNU/Linux\n"
                          "[2026-09-20 10:00:01.150] [root@rv1106:~]# ");
    QVERIFY(writeFile(path, capture));

    QList<bool> replayStates;
    connect(session.get(), &SessionWidget::replayStateChanged, this,
            [&replayStates](bool active) { replayStates.append(active); });

    session->replayLogFile(path, 0);
    QTRY_COMPARE_WITH_TIMEOUT(replayStates.size(), qsizetype(2), kSimTimeoutMs);

    const QString text = allText(session->terminal());
    QVERIFY2(text.contains(QStringLiteral("Linux rv1106 5.10.160 armv7l GNU/Linux")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("[root@rv1106:~]#")));
    // The synthetic line break the logger put before the TX line is not replayed: prompt and
    // echoed command stay on one line as in the live session.
    QVERIFY2(text.contains(QStringLiteral("[root@rv1106:~]# uname -a")), qPrintable(text));
    // The host's own input and the header are not part of the device stream.
    QVERIFY(!text.contains(QStringLiteral("TX>")));
    QVERIFY(!text.contains(QStringLiteral("# BuildAI")));
    QVERIFY(!text.contains(QStringLiteral("2026-09-20 10:00")));
}

// =======================================================================================
// ConnectionBar
// =======================================================================================

void Tst_sessionwidget::barSetPorts()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    auto* portCombo = child<QComboBox>(&bar, "portCombo");
    QVERIFY(portCombo);
    int changes = 0;
    connect(&bar, &ConnectionBar::settingsChanged, this, [&changes](const SerialSettings&) { ++changes; });

    SerialPortEntry ch343 = makeEntry(QStringLiteral("COM8"), QStringLiteral("USB-Enhanced-SERIAL CH343"));
    ch343.manufacturer = QStringLiteral("wch.cn");
    ch343.vendorId = 0x1A86;
    ch343.productId = 0x55D3;
    ch343.hasVidPid = true;
    QList<SerialPortEntry> ports{ch343, makeEntry(QStringLiteral("COM3"))};

    bar.setPorts(ports);
    QCOMPARE(portCombo->count(), 2);
    QCOMPARE(portCombo->itemText(0), QStringLiteral("COM8 - USB-Enhanced-SERIAL CH343"));
    QCOMPARE(portCombo->itemData(0).toString(), QStringLiteral("COM8"));
    QCOMPARE(portCombo->itemText(1), QStringLiteral("COM3"));
    QCOMPARE(portCombo->itemData(1).toString(), QStringLiteral("COM3"));
    const QString tip = portCombo->itemData(0, Qt::ToolTipRole).toString();
    QVERIFY2(tip.contains(QStringLiteral("wch.cn")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("1A86")), qPrintable(tip));
    QVERIFY(bar.selectedPortName().isEmpty()); // nothing selected until the user picks a port
    QCOMPARE(portCombo->currentIndex(), -1);

    bar.selectPort(QStringLiteral("COM3"));
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM3"));
    QCOMPARE(portCombo->currentIndex(), 1);

    // Hot-plug: a new port appears, the selection is kept.
    ports.prepend(makeEntry(QStringLiteral("COM1")));
    bar.setPorts(ports);
    QCOMPARE(portCombo->count(), 3);
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM3"));
    QCOMPARE(portCombo->currentText(), QStringLiteral("COM3"));

    // The selected port vanishes: it stays as a greyed placeholder.
    ports.removeLast();
    bar.setPorts(ports);
    QCOMPARE(portCombo->count(), 3);
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM3"));
    QVERIFY2(portCombo->currentText().contains(QStringLiteral("unavailable")), qPrintable(portCombo->currentText()));

    // It comes back: a normal item again.
    ports.append(makeEntry(QStringLiteral("COM3")));
    bar.setPorts(ports);
    QCOMPARE(portCombo->count(), 3);
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM3"));
    QCOMPARE(portCombo->currentText(), QStringLiteral("COM3"));

    QCOMPARE(changes, 0); // none of this was a user change
}

void Tst_sessionwidget::barSelectMissingPortPlaceholder()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    auto* portCombo = child<QComboBox>(&bar, "portCombo");
    QVERIFY(portCombo);
    bar.setPorts({makeEntry(QStringLiteral("COM8"))});

    bar.selectPort(QStringLiteral("COM42"));
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM42"));
    QCOMPARE(portCombo->count(), 2);
    QCOMPARE(portCombo->currentIndex(), 1);
    QCOMPARE(portCombo->currentText(), QStringLiteral("COM42 (unavailable)"));
    QVERIFY(!portCombo->itemData(1, Qt::ToolTipRole).toString().isEmpty());
    auto* model = qobject_cast<QStandardItemModel*>(portCombo->model());
    QVERIFY(model);
    QVERIFY(!model->item(1)->isEnabled());
    QVERIFY(model->item(0)->isEnabled());
    QCOMPARE(bar.settings().portName, QStringLiteral("COM42"));

    // Selecting a listed port drops the stale placeholder.
    bar.selectPort(QStringLiteral("COM8"));
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM8"));
    QCOMPARE(portCombo->count(), 1);

    // Selecting another missing port replaces rather than accumulates placeholders.
    bar.selectPort(QStringLiteral("COM9"));
    bar.selectPort(QStringLiteral("COM10"));
    QCOMPARE(portCombo->count(), 2);
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM10"));

    // An empty name clears the selection.
    bar.selectPort(QString());
    QVERIFY(bar.selectedPortName().isEmpty());
    QCOMPARE(portCombo->currentIndex(), -1);
    QVERIFY(bar.settings().portName.isEmpty());
}

void Tst_sessionwidget::barSettingsRoundTrip()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    bar.setPorts({makeEntry(QStringLiteral("COM8"))});
    auto* baudCombo = child<QComboBox>(&bar, "baudCombo");
    QVERIFY(baudCombo);
    QVERIFY(baudCombo->isEditable());
    const auto* baudValidator = qobject_cast<const QIntValidator*>(baudCombo->validator());
    QVERIFY(baudValidator);
    QCOMPARE(baudValidator->bottom(), SerialSettings::kMinBaudRate);
    QCOMPARE(baudValidator->top(), SerialSettings::kMaxBaudRate);
    int changes = 0;
    connect(&bar, &ConnectionBar::settingsChanged, this, [&changes](const SerialSettings&) { ++changes; });

    SerialSettings base;
    base.portName = QStringLiteral("COM8");
    base.baudRate = 1500000;
    QVERIFY(roundTrips(bar, base));
    QCOMPARE(baudCombo->currentText(), QStringLiteral("1500000"));

    for (const QSerialPort::Parity parity : {QSerialPort::NoParity, QSerialPort::EvenParity, QSerialPort::OddParity,
                                             QSerialPort::SpaceParity, QSerialPort::MarkParity}) {
        SerialSettings s = base;
        s.parity = parity;
        QVERIFY(roundTrips(bar, s));
    }
    for (const QSerialPort::DataBits bits :
         {QSerialPort::Data5, QSerialPort::Data6, QSerialPort::Data7, QSerialPort::Data8}) {
        SerialSettings s = base;
        s.dataBits = bits;
        QVERIFY(roundTrips(bar, s));
    }
    for (const QSerialPort::StopBits stop : {QSerialPort::OneStop, QSerialPort::OneAndHalfStop, QSerialPort::TwoStop}) {
        SerialSettings s = base;
        s.stopBits = stop;
        QVERIFY(roundTrips(bar, s));
    }
    for (const QSerialPort::FlowControl flow :
         {QSerialPort::NoFlowControl, QSerialPort::HardwareControl, QSerialPort::SoftwareControl}) {
        SerialSettings s = base;
        s.flowControl = flow;
        QVERIFY(roundTrips(bar, s));
    }
    for (const bool dtr : {false, true}) {
        for (const bool rts : {false, true}) {
            SerialSettings s = base;
            s.dtr = dtr;
            s.rts = rts;
            QVERIFY(roundTrips(bar, s));
        }
    }
    for (const qint32 baud : SerialSettings::standardBaudRates()) {
        SerialSettings s = base;
        s.baudRate = baud;
        QVERIFY(roundTrips(bar, s));
        QCOMPARE(baudCombo->currentText(), QString::number(baud));
    }

    // A non-standard rate lives in the edit text only.
    SerialSettings custom = base;
    custom.baudRate = 250000;
    QVERIFY(roundTrips(bar, custom));
    QCOMPARE(baudCombo->currentText(), QStringLiteral("250000"));
    QCOMPARE(baudCombo->currentIndex(), -1);
    QCOMPARE(changes, 0); // setSettings() never emits

    // Typing a rate and pressing Enter is a user change.
    QLineEdit* baudEdit = baudCombo->lineEdit();
    QVERIFY(baudEdit);
    baudEdit->clear();
    QTest::keyClicks(baudEdit, QStringLiteral("921600"));
    QTest::keyClick(baudEdit, Qt::Key_Return);
    QCOMPARE(bar.settings().baudRate, 921600);
    QVERIFY(changes >= 1);

    // Garbage in the edit falls back to 115200.
    baudEdit->clear();
    QCOMPARE(bar.settings().baudRate, 115200);
}

void Tst_sessionwidget::barConnectionStateUi()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    bar.setPorts({makeEntry(QStringLiteral("COM8"))});
    bar.selectPort(QStringLiteral("COM8"));
    auto* button = child<QPushButton>(&bar, "connectButton");
    auto* portCombo = child<QComboBox>(&bar, "portCombo");
    auto* refresh = child<QToolButton>(&bar, "refreshButton");
    auto* breakButton = child<QToolButton>(&bar, "breakButton");
    auto* baudCombo = child<QComboBox>(&bar, "baudCombo");
    QVERIFY(button && portCombo && refresh && breakButton && baudCombo);

    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(portCombo->isEnabled());
    QVERIFY(refresh->isEnabled());
    QVERIFY(!breakButton->isEnabled());
    QVERIFY(!button->icon().isNull());

    bar.setConnectionState(State::Connected);
    QCOMPARE(button->text(), QStringLiteral("Disconnect"));
    QVERIFY(!portCombo->isEnabled());
    QVERIFY(!refresh->isEnabled());
    QVERIFY(breakButton->isEnabled());
    QVERIFY(baudCombo->isEnabled()); // live parameter changes stay possible

    bar.setConnectionState(State::Reconnecting);
    QCOMPARE(button->text(), QStringLiteral("Reconnecting..."));
    QVERIFY(button->isEnabled()); // click = cancel
    QVERIFY(!portCombo->isEnabled());
    QVERIFY(!breakButton->isEnabled());

    bar.setConnectionState(State::Disconnected);
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(portCombo->isEnabled());
    QVERIFY(refresh->isEnabled());
    QVERIFY(!breakButton->isEnabled());
}

void Tst_sessionwidget::barPinToggles()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    auto* dtr = child<QToolButton>(&bar, "dtrButton");
    auto* rts = child<QToolButton>(&bar, "rtsButton");
    QVERIFY(dtr && rts);
    QVERIFY(dtr->isCheckable() && rts->isCheckable());
    QVERIFY(dtr->isChecked()); // asserted by default
    QVERIFY(rts->isChecked());

    QSignalSpy dtrSpy(&bar, &ConnectionBar::dtrToggled);
    QSignalSpy rtsSpy(&bar, &ConnectionBar::rtsToggled);
    int changes = 0;
    connect(&bar, &ConnectionBar::settingsChanged, this, [&changes](const SerialSettings&) { ++changes; });

    QTest::mouseClick(dtr, Qt::LeftButton);
    QVERIFY(!dtr->isChecked());
    QCOMPARE(dtrSpy.count(), 1);
    QCOMPARE(dtrSpy.at(0).at(0).toBool(), false);
    QCOMPARE(rtsSpy.count(), 0);
    QVERIFY(!bar.settings().dtr);
    QVERIFY(bar.settings().rts);
    QCOMPARE(changes, 1);

    QTest::mouseClick(rts, Qt::LeftButton);
    QVERIFY(!rts->isChecked());
    QCOMPARE(rtsSpy.count(), 1);
    QCOMPARE(rtsSpy.at(0).at(0).toBool(), false);
    QVERIFY(!bar.settings().rts);
    QCOMPARE(changes, 2);

    QTest::mouseClick(dtr, Qt::LeftButton);
    QCOMPARE(dtrSpy.count(), 2);
    QCOMPARE(dtrSpy.at(1).at(0).toBool(), true);
    QVERIFY(bar.settings().dtr);

    // Mirroring the connection's pins is silent.
    bar.setPinStates(false, true);
    QVERIFY(!dtr->isChecked());
    QVERIFY(rts->isChecked());
    QCOMPARE(dtrSpy.count(), 2);
    QCOMPARE(rtsSpy.count(), 1);
    QCOMPARE(changes, 3);
}

void Tst_sessionwidget::barBreakButton()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    auto* breakButton = child<QToolButton>(&bar, "breakButton");
    QVERIFY(breakButton);
    QCOMPARE(breakButton->text(), QStringLiteral("Break"));
    QSignalSpy breakSpy(&bar, &ConnectionBar::sendBreakRequested);

    QVERIFY(!breakButton->isEnabled()); // disconnected
    bar.setConnectionState(State::Connected);
    QVERIFY(breakButton->isEnabled());
    QTest::mouseClick(breakButton, Qt::LeftButton);
    QCOMPARE(breakSpy.count(), 1);
    QTest::mouseClick(breakButton, Qt::LeftButton);
    QCOMPARE(breakSpy.count(), 2);

    bar.setConnectionState(State::Disconnected);
    QVERIFY(!breakButton->isEnabled());
}

void Tst_sessionwidget::barSettingsChangedOnlyByUser()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    QList<SerialSettings> changes;
    connect(&bar, &ConnectionBar::settingsChanged, this,
            [&changes](const SerialSettings& settings) { changes.append(settings); });

    const QList<SerialPortEntry> ports{makeEntry(QStringLiteral("COM8")), makeEntry(QStringLiteral("COM3"))};
    bar.setPorts(ports);
    SerialSettings s;
    s.portName = QStringLiteral("COM3");
    s.baudRate = 9600;
    s.parity = QSerialPort::EvenParity;
    s.dtr = false;
    bar.setSettings(s);
    bar.setPorts(ports);
    bar.selectPort(QStringLiteral("COM8"));
    bar.setPinStates(true, false);
    bar.setConnectionState(State::Connected);
    bar.setConnectionState(State::Disconnected);
    QVERIFY(changes.isEmpty());
    QCOMPARE(bar.selectedPortName(), QStringLiteral("COM8"));

    auto* parityCombo = child<QComboBox>(&bar, "parityCombo");
    auto* flowCombo = child<QComboBox>(&bar, "flowCombo");
    auto* dataBitsCombo = child<QComboBox>(&bar, "dataBitsCombo");
    auto* stopBitsCombo = child<QComboBox>(&bar, "stopBitsCombo");
    auto* baudCombo = child<QComboBox>(&bar, "baudCombo");
    auto* portCombo = child<QComboBox>(&bar, "portCombo");
    QVERIFY(parityCombo && flowCombo && dataBitsCombo && stopBitsCombo && baudCombo && portCombo);

    parityCombo->setCurrentIndex(parityCombo->findData(static_cast<int>(QSerialPort::OddParity)));
    QCOMPARE(changes.size(), qsizetype(1));
    QCOMPARE(changes.last().parity, QSerialPort::OddParity);
    QCOMPARE(changes.last().portName, QStringLiteral("COM8"));
    QCOMPARE(changes.last().baudRate, 9600);

    flowCombo->setCurrentIndex(flowCombo->findData(static_cast<int>(QSerialPort::HardwareControl)));
    QCOMPARE(changes.size(), qsizetype(2));
    QCOMPARE(changes.last().flowControl, QSerialPort::HardwareControl);

    dataBitsCombo->setCurrentIndex(dataBitsCombo->findData(static_cast<int>(QSerialPort::Data7)));
    QCOMPARE(changes.size(), qsizetype(3));
    QCOMPARE(changes.last().dataBits, QSerialPort::Data7);

    stopBitsCombo->setCurrentIndex(stopBitsCombo->findData(static_cast<int>(QSerialPort::TwoStop)));
    QCOMPARE(changes.size(), qsizetype(4));
    QCOMPARE(changes.last().stopBits, QSerialPort::TwoStop);

    baudCombo->setCurrentIndex(baudCombo->findData(230400));
    QCOMPARE(changes.size(), qsizetype(5));
    QCOMPARE(changes.last().baudRate, 230400);

    portCombo->setCurrentIndex(1);
    QCOMPARE(changes.size(), qsizetype(6));
    QCOMPARE(changes.last().portName, QStringLiteral("COM3"));
    QCOMPARE(changes.last().summary(), QStringLiteral("230400 7O2 RTS/CTS"));
    QVERIFY(changes.last() == bar.settings());
}

void Tst_sessionwidget::barButtonsEmitRequests()
{
    ConnectionBar bar;
    QVERIFY(expose(&bar));
    bar.setPorts({makeEntry(QStringLiteral("COM8"))});
    bar.selectPort(QStringLiteral("COM8"));
    auto* button = child<QPushButton>(&bar, "connectButton");
    auto* refresh = child<QToolButton>(&bar, "refreshButton");
    QVERIFY(button && refresh);
    QSignalSpy connectSpy(&bar, &ConnectionBar::connectRequested);
    QSignalSpy disconnectSpy(&bar, &ConnectionBar::disconnectRequested);
    QSignalSpy refreshSpy(&bar, &ConnectionBar::refreshRequested);

    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(disconnectSpy.count(), 0);

    bar.setConnectionState(State::Connected);
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(disconnectSpy.count(), 1);

    bar.setConnectionState(State::Reconnecting);
    QTest::mouseClick(button, Qt::LeftButton); // cancels the reconnect
    QCOMPARE(disconnectSpy.count(), 2);

    bar.setConnectionState(State::Disconnected);
    QTest::mouseClick(refresh, Qt::LeftButton);
    QCOMPARE(refreshSpy.count(), 1);

    bar.setFocusToPort();
    QCOMPARE(bar.focusWidget(), static_cast<QWidget*>(child<QComboBox>(&bar, "portCombo")));
}

// =======================================================================================
// CommandInput
// =======================================================================================

void Tst_sessionwidget::inputInvalidHex()
{
    CommandInput input;
    QVERIFY(expose(&input));
    input.setEnabledForConnection(true);
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    QVERIFY(edit);
    const QString normalTip = edit->toolTip();
    QVERIFY(!normalTip.isEmpty());
    QSignalSpy spy(&input, &CommandInput::sendRequested);

    input.setHexMode(true);
    input.setText(QStringLiteral("4G"));
    input.send();
    QCOMPARE(spy.count(), 0);
    QVERIFY2(edit->styleSheet().contains(QStringLiteral("E74C3C")), qPrintable(edit->styleSheet()));
    QVERIFY(edit->toolTip() != normalTip);
    QVERIFY2(edit->toolTip().contains(QLatin1Char('G')) || edit->toolTip().contains(QStringLiteral("nvalid")),
             qPrintable(edit->toolTip()));
    QCOMPARE(input.text(), QStringLiteral("4G")); // kept for correction

    // Editing the text clears the error feedback.
    QTest::keyClicks(edit, QStringLiteral("1"));
    QVERIFY(edit->styleSheet().isEmpty());
    QCOMPARE(edit->toolTip(), normalTip);

    // Odd number of nibbles is an error too.
    input.setText(QStringLiteral("ABC"));
    input.send();
    QCOMPARE(spy.count(), 0);
    QVERIFY(!edit->styleSheet().isEmpty());

    // Valid hex sends the bytes without a line ending and clears the field.
    input.setText(QStringLiteral("0x41,0x42 0d"));
    input.send();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("AB\r"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("0x41,0x42 0d"));
    QVERIFY(input.text().isEmpty());
    QVERIFY(edit->styleSheet().isEmpty());

    // Whitespace-only hex sends nothing and is not an error.
    input.setText(QStringLiteral("   "));
    input.send();
    QCOMPARE(spy.count(), 1);
    QVERIFY(edit->styleSheet().isEmpty());
}

void Tst_sessionwidget::inputHexModeDisablesControls()
{
    CommandInput input;
    QVERIFY(expose(&input));
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    auto* combo = child<QComboBox>(&input, "lineEndingCombo");
    auto* hexCheck = child<QCheckBox>(&input, "hexCheck");
    auto* escapeCheck = child<QCheckBox>(&input, "escapeCheck");
    QVERIFY(edit && combo && hexCheck && escapeCheck);
    QVERIFY(combo->isEnabled());
    QVERIFY(escapeCheck->isEnabled());
    QVERIFY(!input.hexMode());
    const QString textPlaceholder = edit->placeholderText();

    // The placeholder explains the current mode: plain / escapes / HEX (hex wins over a still-checked
    // escape box, matching the precedence in send()).
    input.setEscapeMode(true);
    QVERIFY2(edit->placeholderText().contains(QStringLiteral("escape"), Qt::CaseInsensitive),
             qPrintable(edit->placeholderText()));
    QVERIFY(edit->placeholderText() != textPlaceholder);
    input.setHexMode(true);
    QVERIFY(edit->placeholderText().contains(QStringLiteral("Hex")));
    input.setHexMode(false);
    QVERIFY(edit->placeholderText().contains(QStringLiteral("escape"), Qt::CaseInsensitive));
    input.setEscapeMode(false);
    QCOMPARE(edit->placeholderText(), textPlaceholder);

    input.setHexMode(true);
    QVERIFY(hexCheck->isChecked());
    QVERIFY(!combo->isEnabled());
    QVERIFY(!escapeCheck->isEnabled());
    QVERIFY2(edit->placeholderText().contains(QStringLiteral("Hex")), qPrintable(edit->placeholderText()));

    input.setHexMode(false);
    QVERIFY(combo->isEnabled());
    QVERIFY(escapeCheck->isEnabled());
    QCOMPARE(edit->placeholderText(), textPlaceholder);

    // Through the checkbox itself.
    QTest::mouseClick(hexCheck, Qt::LeftButton);
    QVERIFY(input.hexMode());
    QVERIFY(!combo->isEnabled());
    QVERIFY(!escapeCheck->isEnabled());
    QTest::mouseClick(hexCheck, Qt::LeftButton);
    QVERIFY(!input.hexMode());
    QVERIFY(combo->isEnabled());
}

void Tst_sessionwidget::inputCtrlLClears()
{
    CommandInput input;
    QVERIFY(expose(&input));
    input.setEnabledForConnection(true);
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    QVERIFY(edit);

    QTest::keyClicks(edit, QStringLiteral("some text"));
    QCOMPARE(input.text(), QStringLiteral("some text"));
    QTest::keyClick(edit, Qt::Key_L, Qt::ControlModifier);
    QVERIFY(input.text().isEmpty());

    // A plain 'l' is ordinary text.
    QTest::keyClick(edit, Qt::Key_L);
    QCOMPARE(input.text(), QStringLiteral("l"));

    // Ctrl+L also clears the error feedback.
    input.setHexMode(true);
    input.setText(QStringLiteral("zz"));
    input.send();
    QVERIFY(!edit->styleSheet().isEmpty());
    QTest::keyClick(edit, Qt::Key_L, Qt::ControlModifier);
    QVERIFY(input.text().isEmpty());
    QVERIFY(edit->styleSheet().isEmpty());
}

void Tst_sessionwidget::inputEscapeClears()
{
    CommandInput input;
    QVERIFY(expose(&input));
    CommandHistory history;
    history.add(QStringLiteral("older"));
    input.setHistory(&history);
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    QVERIFY(edit);

    QTest::keyClicks(edit, QStringLiteral("abc"));
    QCOMPARE(input.text(), QStringLiteral("abc"));
    QTest::keyClick(edit, Qt::Key_Escape);
    QVERIFY(input.text().isEmpty());

    // Escape while browsing the history drops the recalled entry and resets navigation.
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input.text(), QStringLiteral("older"));
    QVERIFY(history.isNavigating());
    QTest::keyClick(edit, Qt::Key_Escape);
    QVERIFY(input.text().isEmpty());
    QVERIFY(!history.isNavigating());

    // Down after Escape does nothing (the blank line is the bottom).
    QTest::keyClick(edit, Qt::Key_Down);
    QVERIFY(input.text().isEmpty());
}

void Tst_sessionwidget::inputHistoryDraftRestore()
{
    CommandInput input;
    QVERIFY(expose(&input));
    CommandHistory history;
    history.add(QStringLiteral("first"));
    history.add(QStringLiteral("second"));
    input.setHistory(&history);
    QCOMPARE(input.history(), &history);
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    QVERIFY(edit);

    QTest::keyClicks(edit, QStringLiteral("draft"));
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input.text(), QStringLiteral("second"));
    QCOMPARE(edit->cursorPosition(), 6); // cursor at the end of the recalled entry
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input.text(), QStringLiteral("first"));
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input.text(), QStringLiteral("first"));
    QTest::keyClick(edit, Qt::Key_Down);
    QCOMPARE(input.text(), QStringLiteral("second"));
    QTest::keyClick(edit, Qt::Key_Down);
    QCOMPARE(input.text(), QStringLiteral("draft"));
    QVERIFY(!history.isNavigating());
    QTest::keyClick(edit, Qt::Key_Down); // nothing below the draft
    QCOMPARE(input.text(), QStringLiteral("draft"));

    // Sending appends to the history and a fresh Up recalls it.
    input.setEnabledForConnection(true);
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(history.entries(),
             (QStringList{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("draft")}));
    QVERIFY(input.text().isEmpty());
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input.text(), QStringLiteral("draft"));
    QTest::keyClick(edit, Qt::Key_Down);
    QVERIFY(input.text().isEmpty()); // the draft was empty this time

    // Without a history the arrows are ordinary line-edit keys.
    input.setHistory(nullptr);
    QVERIFY(!input.history());
    QTest::keyClicks(edit, QStringLiteral("x"));
    QTest::keyClick(edit, Qt::Key_Up);
    QCOMPARE(input.text(), QStringLiteral("x"));
}

void Tst_sessionwidget::inputEnterDisconnectedNoEmit()
{
    CommandInput input;
    QVERIFY(expose(&input));
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    auto* sendButton = child<QPushButton>(&input, "sendButton");
    QVERIFY(edit && sendButton);
    QSignalSpy spy(&input, &CommandInput::sendRequested);

    input.setEnabledForConnection(false);
    QVERIFY(!sendButton->isEnabled());
    QVERIFY(edit->isEnabled()); // typing stays possible
    QTest::keyClicks(edit, QStringLiteral("hello"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(input.text(), QStringLiteral("hello"));
    QVERIFY(!edit->styleSheet().isEmpty());
    QCOMPARE(edit->toolTip(), QStringLiteral("Not connected"));
    input.send();
    QCOMPARE(spy.count(), 0);

    input.setEnabledForConnection(true);
    QVERIFY(sendButton->isEnabled());
    QVERIFY(edit->styleSheet().isEmpty()); // the "Not connected" feedback is gone
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("hello\r"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("hello"));
    QVERIFY(input.text().isEmpty());

    QTest::keyClicks(edit, QStringLiteral("again"));
    QTest::mouseClick(sendButton, Qt::LeftButton);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toByteArray(), QByteArrayLiteral("again\r"));
}

void Tst_sessionwidget::inputLineEndingChanged()
{
    CommandInput input;
    QVERIFY(expose(&input));
    input.setEnabledForConnection(true);
    auto* combo = child<QComboBox>(&input, "lineEndingCombo");
    QVERIFY(combo);
    QCOMPARE(combo->count(), LineEnding::allModes().size());
    QVERIFY(input.lineEnding() == LineEnding::Mode::CR);

    QList<int> modes;
    connect(&input, &CommandInput::lineEndingChanged, this,
            [&modes](LineEnding::Mode mode) { modes.append(static_cast<int>(mode)); });
    QSignalSpy spy(&input, &CommandInput::sendRequested);

    combo->setCurrentIndex(combo->findData(static_cast<int>(LineEnding::Mode::CRLF)));
    QCOMPARE(modes, QList<int>{static_cast<int>(LineEnding::Mode::CRLF)});
    QVERIFY(input.lineEnding() == LineEnding::Mode::CRLF);
    input.setText(QStringLiteral("AT"));
    input.send();
    QCOMPARE(spy.last().at(0).toByteArray(), QByteArrayLiteral("AT\r\n"));

    input.setLineEnding(LineEnding::Mode::LF);
    QCOMPARE(modes.size(), qsizetype(2));
    QCOMPARE(modes.last(), static_cast<int>(LineEnding::Mode::LF));
    QCOMPARE(combo->currentData().toInt(), static_cast<int>(LineEnding::Mode::LF));
    input.setLineEnding(LineEnding::Mode::LF); // unchanged: no signal
    QCOMPARE(modes.size(), qsizetype(2));
    input.setText(QStringLiteral("AT"));
    input.send();
    QCOMPARE(spy.last().at(0).toByteArray(), QByteArrayLiteral("AT\n"));

    input.setLineEnding(LineEnding::Mode::None);
    input.setText(QStringLiteral("AT"));
    input.send();
    QCOMPARE(spy.last().at(0).toByteArray(), QByteArrayLiteral("AT"));
    QCOMPARE(spy.count(), 3);
}

void Tst_sessionwidget::inputEscapePayload()
{
    CommandInput input;
    QVERIFY(expose(&input));
    input.setEnabledForConnection(true);
    auto* edit = child<QLineEdit>(&input, "commandEdit");
    QVERIFY(edit);
    QSignalSpy spy(&input, &CommandInput::sendRequested);

    input.setEscapeMode(true);
    input.setLineEnding(LineEnding::Mode::None);
    input.setText(QStringLiteral("AT\\r\\n\\x1b[A\\u00e9"));
    input.send();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("AT\r\n\x1b[A\xc3\xa9"));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("AT\\r\\n\\x1b[A\\u00e9"));

    // With a line ending it is appended after the unescaped bytes.
    input.setLineEnding(LineEnding::Mode::CR);
    input.setText(QStringLiteral("\\x03"));
    input.send();
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toByteArray(), QByteArrayLiteral("\x03\r"));

    // An unknown escape is rejected with feedback and nothing is sent.
    input.setText(QStringLiteral("bad\\q"));
    input.send();
    QCOMPARE(spy.count(), 2);
    QVERIFY(!edit->styleSheet().isEmpty());
    QCOMPARE(input.text(), QStringLiteral("bad\\q"));

    // Escape mode off: backslashes are literal text.
    input.setEscapeMode(false);
    input.setText(QStringLiteral("a\\tb"));
    input.send();
    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.at(2).at(0).toByteArray(), QByteArrayLiteral("a\\tb\r"));
}

// =======================================================================================
// QuickCommandBar
// =======================================================================================

void Tst_sessionwidget::quickBarButtonsMatchGroup()
{
    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    QCOMPARE(bar.store(), m_store);
    QCOMPARE(bar.currentGroup(), QString()); // "All"
    auto* combo = child<QComboBox>(&bar, "groupCombo");
    QVERIFY(combo);
    QCOMPARE(combo->itemText(0), QStringLiteral("All"));
    const QStringList groups = m_store->groups();
    QCOMPARE(combo->count(), static_cast<int>(groups.size()) + 1);
    for (int i = 0; i < groups.size(); ++i) {
        QCOMPARE(combo->itemText(i + 1), groups.at(i));
        QCOMPARE(combo->itemData(i + 1).toString(), groups.at(i));
    }

    const QList<QuickCommand> all = m_store->commands();
    QList<QToolButton*> buttons = commandButtons(&bar);
    QCOMPARE(buttonTexts(buttons), commandNames(all));
    QVERIFY(buttons.first()->toolTip().contains(QStringLiteral("uname -a")));
    QVERIFY(buttons.first()->toolTip().contains(QStringLiteral("CR")));

    bar.setCurrentGroup(QStringLiteral("U-Boot"));
    QCOMPARE(bar.currentGroup(), QStringLiteral("U-Boot"));
    buttons = commandButtons(&bar);
    QCOMPARE(buttonTexts(buttons), commandNames(all, QStringLiteral("U-Boot")));
    QVERIFY(!buttons.isEmpty());

    bar.setCurrentGroup(QStringLiteral("Control"));
    buttons = commandButtons(&bar);
    QCOMPARE(buttonTexts(buttons), commandNames(all, QStringLiteral("Control")));
    QCOMPARE(buttons.first()->text(), QStringLiteral("Ctrl+C"));
    QVERIFY2(buttons.first()->toolTip().contains(QStringLiteral("03")), qPrintable(buttons.first()->toolTip()));
    QVERIFY(buttons.first()->toolTip().contains(QStringLiteral("HEX")));

    bar.setCurrentGroup(QString());
    QCOMPARE(bar.currentGroup(), QString());
    QCOMPARE(buttonTexts(commandButtons(&bar)), commandNames(all));
}

void Tst_sessionwidget::quickBarClickEmits()
{
    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    bar.setEnabledForConnection(true);
    QList<QuickCommand> triggered;
    connect(&bar, &QuickCommandBar::commandTriggered, this,
            [&triggered](const QuickCommand& command) { triggered.append(command); });
    QSignalSpy editSpy(&bar, &QuickCommandBar::editRequested);

    const QList<QuickCommand> all = m_store->commands();
    QList<QToolButton*> buttons = commandButtons(&bar);
    QVERIFY(buttons.size() >= 2);
    QVERIFY(buttons.first()->width() > 0);
    QTest::mouseClick(buttons.first(), Qt::LeftButton);
    QCOMPARE(triggered.size(), qsizetype(1));
    QVERIFY(triggered.first() == all.first());
    QCOMPARE(triggered.first().command, QStringLiteral("uname -a"));

    QTest::mouseClick(buttons.at(1), Qt::LeftButton);
    QCOMPARE(triggered.size(), qsizetype(2));
    QVERIFY(triggered.last() == all.at(1));

    // A hex control command carries its raw bytes.
    bar.setCurrentGroup(QStringLiteral("Control"));
    buttons = commandButtons(&bar);
    QToolButton* ctrlC = nullptr;
    for (QToolButton* button : buttons) {
        if (button->text() == QStringLiteral("Ctrl+C")) {
            ctrlC = button;
        }
    }
    QVERIFY(ctrlC);
    QTest::mouseClick(ctrlC, Qt::LeftButton);
    QCOMPARE(triggered.size(), qsizetype(3));
    QVERIFY(triggered.last().hex);
    QCOMPARE(triggered.last().command, QStringLiteral("03"));
    QCOMPARE(triggered.last().payload(), QByteArrayLiteral("\x03"));
    QCOMPARE(editSpy.count(), 0);
}

void Tst_sessionwidget::quickBarGroupFilterPersists()
{
    QVERIFY(QSettings().value(kGroupKey).toString().isEmpty());
    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "groupCombo");
    QVERIFY(combo);
    const QList<QuickCommand> all = m_store->commands();

    bar.setCurrentGroup(QStringLiteral("Linux"));
    QCOMPARE(bar.currentGroup(), QStringLiteral("Linux"));
    QCOMPARE(QSettings().value(kGroupKey).toString(), QStringLiteral("Linux"));
    QCOMPARE(buttonTexts(commandButtons(&bar)), commandNames(all, QStringLiteral("Linux")));

    // A bar created later restores the remembered group.
    QuickCommandBar second(m_store);
    QCOMPARE(second.currentGroup(), QStringLiteral("Linux"));
    QCOMPARE(buttonTexts(commandButtons(&second)), commandNames(all, QStringLiteral("Linux")));

    // A user pick in the combo filters and persists too.
    combo->setCurrentIndex(combo->findData(QStringLiteral("MCU")));
    QCOMPARE(bar.currentGroup(), QStringLiteral("MCU"));
    QCOMPARE(QSettings().value(kGroupKey).toString(), QStringLiteral("MCU"));
    QCOMPARE(buttonTexts(commandButtons(&bar)), commandNames(all, QStringLiteral("MCU")));
    QCOMPARE(second.currentGroup(), QStringLiteral("Linux")); // the other bar keeps its own group

    // A store change rebuilds every bar, each with its own group (not the last persisted one).
    QList<QuickCommand> edited = all;
    edited.first().name = QStringLiteral("renamed");
    m_store->setCommands(edited);
    QCOMPARE(bar.currentGroup(), QStringLiteral("MCU"));
    QCOMPARE(second.currentGroup(), QStringLiteral("Linux"));
    QCOMPARE(buttonTexts(commandButtons(&second)), commandNames(edited, QStringLiteral("Linux")));
    QuickCommandBar third(m_store);
    QCOMPARE(third.currentGroup(), QStringLiteral("MCU")); // a new bar starts from the last chosen group

    // An unknown group means "All".
    bar.setCurrentGroup(QStringLiteral("Nope"));
    QCOMPARE(bar.currentGroup(), QString());
    QCOMPARE(combo->currentIndex(), 0);
    QVERIFY(QSettings().value(kGroupKey).toString().isEmpty());
    QCOMPARE(buttonTexts(commandButtons(&bar)), commandNames(edited));
}

void Tst_sessionwidget::quickBarGroupTranslated()
{
    // The general group is stored under one untranslated key and only displayed translated.
    QTranslator translator;
    QVERIFY(translator.load(QStringLiteral(":/translations/zh_CN.qm")));
    QVERIFY(qApp->installTranslator(&translator));
    const QString general = QStringLiteral(u"常规"); // zh_CN translation of "General"

    const QList<QuickCommand> custom{
        makeCommand(QStringLiteral("hello"), QStringLiteral("echo hello"), QStringLiteral("Test")),
        makeCommand(QStringLiteral("AT"), QStringLiteral("AT"), QString())};
    m_store->setCommands(custom);
    QCOMPARE(m_store->groups(), (QStringList{QStringLiteral("Test"), QStringLiteral("General")}));

    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "groupCombo");
    QVERIFY(combo);
    QCOMPARE(combo->count(), 3);
    QCOMPARE(combo->itemText(1), QStringLiteral("Test"));
    QCOMPARE(combo->itemText(2), general);
    QCOMPARE(combo->itemData(2).toString(), QStringLiteral("General"));

    bar.setCurrentGroup(QStringLiteral("General"));
    QCOMPARE(bar.currentGroup(), QStringLiteral("General"));
    QCOMPARE(combo->currentText(), general);
    QCOMPARE(buttonTexts(commandButtons(&bar)), QStringList{QStringLiteral("AT")});
    QCOMPARE(QSettings().value(kGroupKey).toString(), QStringLiteral("General"));

    // Switching the language back relabels the item without a rebuild; the group is kept.
    QVERIFY(qApp->removeTranslator(&translator));
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(&bar, &languageChange);
    QCOMPARE(combo->itemText(2), QStringLiteral("General"));
    QCOMPARE(combo->itemData(2).toString(), QStringLiteral("General"));
    QCOMPARE(bar.currentGroup(), QStringLiteral("General"));
    QCOMPARE(buttonTexts(commandButtons(&bar)), QStringList{QStringLiteral("AT")});
    QCOMPARE(QSettings().value(kGroupKey).toString(), QStringLiteral("General"));
}

void Tst_sessionwidget::quickBarGearEmitsEdit()
{
    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    bar.setEnabledForConnection(true);
    QSignalSpy editSpy(&bar, &QuickCommandBar::editRequested);
    QList<QuickCommand> triggered;
    connect(&bar, &QuickCommandBar::commandTriggered, this,
            [&triggered](const QuickCommand& command) { triggered.append(command); });

    auto* gear = child<QToolButton>(&bar, "editButton");
    QVERIFY(gear);
    QVERIFY(gear->isEnabled());
    QTest::mouseClick(gear, Qt::LeftButton);
    QCOMPARE(editSpy.count(), 1);

    // Middle-click on a command button edits instead of sending.
    const QList<QToolButton*> buttons = commandButtons(&bar);
    QVERIFY(!buttons.isEmpty());
    QTest::mouseClick(buttons.first(), Qt::MiddleButton);
    QCOMPARE(editSpy.count(), 2);
    QVERIFY(triggered.isEmpty());

    // The gear works while disconnected as well.
    bar.setEnabledForConnection(false);
    QVERIFY(gear->isEnabled());
    QTest::mouseClick(gear, Qt::LeftButton);
    QCOMPARE(editSpy.count(), 3);
}

void Tst_sessionwidget::quickBarRebuildOnStoreChanged()
{
    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "groupCombo");
    QVERIFY(combo);
    QCOMPARE(commandButtons(&bar).size(), QuickCommandStore::defaults().size());

    QList<QuickCommand> custom{
        makeCommand(QStringLiteral("hello"), QStringLiteral("echo hello"), QStringLiteral("Test")),
        makeCommand(QStringLiteral("bye"), QStringLiteral("exit"), QStringLiteral("Test")),
        makeCommand(QStringLiteral("AT"), QStringLiteral("AT"), QString())};
    m_store->setCommands(custom);
    QCOMPARE(buttonTexts(commandButtons(&bar)),
             (QStringList{QStringLiteral("hello"), QStringLiteral("bye"), QStringLiteral("AT")}));
    QCOMPARE(combo->count(), 3);
    QCOMPARE(combo->itemText(1), QStringLiteral("Test"));
    QCOMPARE(combo->itemText(2), QStringLiteral("General")); // the empty group

    bar.setCurrentGroup(QStringLiteral("General"));
    QCOMPARE(buttonTexts(commandButtons(&bar)), QStringList{QStringLiteral("AT")});

    // A store change keeps the active filter.
    custom.append(makeCommand(QStringLiteral("date"), QStringLiteral("date"), QString()));
    m_store->setCommands(custom);
    QCOMPARE(bar.currentGroup(), QStringLiteral("General"));
    QCOMPARE(buttonTexts(commandButtons(&bar)), (QStringList{QStringLiteral("AT"), QStringLiteral("date")}));

    // A button without a name shows its command text.
    QuickCommand unnamed;
    unnamed.command = QStringLiteral("ls -l");
    custom.append(unnamed);
    m_store->setCommands(custom);
    QCOMPARE(buttonTexts(commandButtons(&bar)).last(), QStringLiteral("ls -l"));

    // The remembered group disappears: back to All, the setting is kept for when it returns.
    m_store->setCommands({custom.first()});
    QCOMPARE(bar.currentGroup(), QString());
    QCOMPARE(buttonTexts(commandButtons(&bar)), QStringList{QStringLiteral("hello")});
    QCOMPARE(QSettings().value(kGroupKey).toString(), QStringLiteral("General"));

    m_store->setCommands(custom);
    QCOMPARE(bar.currentGroup(), QStringLiteral("General"));
}

void Tst_sessionwidget::quickBarEnabledForConnection()
{
    QuickCommandBar bar(m_store);
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "groupCombo");
    auto* gear = child<QToolButton>(&bar, "editButton");
    QVERIFY(combo && gear);
    QList<QuickCommand> triggered;
    connect(&bar, &QuickCommandBar::commandTriggered, this,
            [&triggered](const QuickCommand& command) { triggered.append(command); });

    auto allEnabled = [](const QList<QToolButton*>& buttons, bool enabled) {
        for (const QToolButton* button : buttons) {
            if (button->isEnabled() != enabled) {
                return false;
            }
        }
        return !buttons.isEmpty();
    };

    // Disabled by default (no connection yet); the combo and gear stay usable.
    QVERIFY(allEnabled(commandButtons(&bar), false));
    QVERIFY(combo->isEnabled());
    QVERIFY(gear->isEnabled());

    bar.setEnabledForConnection(true);
    QVERIFY(allEnabled(commandButtons(&bar), true));

    bar.setEnabledForConnection(false);
    QVERIFY(allEnabled(commandButtons(&bar), false));
    QVERIFY(combo->isEnabled());
    QVERIFY(gear->isEnabled());

    // Buttons created by a rebuild inherit the state.
    bar.setCurrentGroup(QStringLiteral("MCU"));
    QVERIFY(allEnabled(commandButtons(&bar), false));
    bar.setEnabledForConnection(true);
    bar.setCurrentGroup(QStringLiteral("Linux"));
    QVERIFY(allEnabled(commandButtons(&bar), true));

    // A disabled button emits nothing when clicked.
    bar.setEnabledForConnection(false);
    QTest::mouseClick(commandButtons(&bar).first(), Qt::LeftButton);
    QVERIFY(triggered.isEmpty());
    bar.setEnabledForConnection(true);
    QTest::mouseClick(commandButtons(&bar).first(), Qt::LeftButton);
    QCOMPARE(triggered.size(), qsizetype(1));
}

QTEST_MAIN(Tst_sessionwidget)
#include "tst_sessionwidget.moc"
