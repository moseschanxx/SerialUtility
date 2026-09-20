#include "ui/SessionWidget.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QStackedWidget>
#include <QStringConverter>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QVBoxLayout>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/CommandHistory.h"
#include "core/HexUtils.h"
#include "core/LineEnding.h"
#include "core/LogReplayer.h"
#include "core/SessionLogger.h"
#include "dialogs/SendFileDialog.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalTheme.h"
#include "terminal/TerminalWidget.h"
#include "ui/CommandInput.h"
#include "ui/ConnectionBar.h"
#include "ui/HexDumpView.h"
#include "ui/QuickCommandBar.h"

namespace {

constexpr int kStatusShortMs = 3000;
constexpr int kStatusLongMs = 5000;

} // namespace

SessionWidget::SessionWidget(QuickCommandStore* quickCommands, CommandHistory* history, QWidget* parent)
    : QWidget(parent)
    , m_quickCommands(quickCommands)
    , m_history(history)
{
    m_connection = new SerialConnection(this);
    m_logger = new SessionLogger(this);

    setupUi();

    // Start from the user's default line parameters (no port selected yet).
    SerialSettings defaults = AppSettings::instance().defaultSerialSettings();
    defaults.portName.clear();
    m_bar->setSettings(defaults);
    m_connection->setSettings(defaults);

    // ---- Connection -> views / logger ---------------------------------------------------
    connect(m_connection, &SerialConnection::dataReceived, m_terminal, &TerminalWidget::feedData);
    connect(m_connection, &SerialConnection::dataReceived, m_hexView, &HexDumpView::appendReceived);
    connect(m_connection, &SerialConnection::dataReceived, m_logger, &SessionLogger::logReceived);
    connect(m_connection, &SerialConnection::dataSent, m_hexView, &HexDumpView::appendSent);
    connect(m_connection, &SerialConnection::dataSent, m_logger, &SessionLogger::logSent);
    connect(m_connection, &SerialConnection::stateChanged, this, &SessionWidget::onConnectionStateChanged);
    connect(m_connection, &SerialConnection::errorOccurred, this, &SessionWidget::onConnectionError);
    connect(m_connection, &SerialConnection::portDisappeared, this, &SessionWidget::onPortDisappeared);
    connect(m_connection, &SerialConnection::reconnected, this, &SessionWidget::onReconnected);
    connect(m_connection, &SerialConnection::countersChanged, this, &SessionWidget::countersChanged);
    connect(m_connection, &SerialConnection::pinsChanged, m_bar, &ConnectionBar::setPinStates);

    // ---- Terminal -----------------------------------------------------------------------
    connect(m_terminal, &TerminalWidget::sendData, this, &SessionWidget::sendBytes);
    connect(m_terminal, &TerminalWidget::fileDropped, this, [this](const QString& path) { sendFile(path); });
    connect(m_terminal, &TerminalWidget::syncSizeRequested, this, &SessionWidget::syncTerminalSize);
    connect(m_terminal, &TerminalWidget::findRequested, this, &SessionWidget::findRequested);
    connect(m_terminal, &TerminalWidget::gridSizeChanged, this, &SessionWidget::gridSizeChanged);
    connect(m_terminal, &TerminalWidget::titleChanged, this, [this](const QString& deviceTitle) {
        if (!deviceTitle.trimmed().isEmpty()) {
            emit statusMessage(deviceTitle, kStatusShortMs);
        }
    });

    // ---- Command input / quick commands ---------------------------------------------------
    connect(m_input, &CommandInput::sendRequested, this, &SessionWidget::onInputSendRequested);
    connect(m_quickBar, &QuickCommandBar::commandTriggered, this, &SessionWidget::sendQuickCommand);
    connect(m_quickBar, &QuickCommandBar::editRequested, this, &SessionWidget::quickCommandsEditRequested);

    // ---- Connection bar -------------------------------------------------------------------
    connect(m_bar, &ConnectionBar::connectRequested, this, [this]() { connectPort(); });
    connect(m_bar, &ConnectionBar::disconnectRequested, this, &SessionWidget::disconnectPort);
    connect(m_bar, &ConnectionBar::settingsChanged, this, &SessionWidget::onBarSettingsChanged);
    connect(m_bar, &ConnectionBar::dtrToggled, m_connection, &SerialConnection::setDtr);
    connect(m_bar, &ConnectionBar::rtsToggled, m_connection, &SerialConnection::setRts);
    connect(m_bar, &ConnectionBar::sendBreakRequested, this, &SessionWidget::sendBreak);
    connect(m_bar, &ConnectionBar::refreshRequested, &SerialPortEnumerator::instance(), &SerialPortEnumerator::refresh);
    connect(&SerialPortEnumerator::instance(), &SerialPortEnumerator::portsChanged, this,
            &SessionWidget::onPortsChanged);

    // ---- Logger ---------------------------------------------------------------------------
    connect(m_logger, &SessionLogger::started, this, [this](const QString& filePath) {
        qCInfo(lcApp) << "session log started:" << filePath;
        emit loggingChanged(true, filePath);
        emit statusMessage(tr("Logging to %1").arg(QDir::toNativeSeparators(filePath)), kStatusShortMs);
    });
    connect(m_logger, &SessionLogger::stopped, this, [this](const QString& filePath, qint64 bytesWritten) {
        qCInfo(lcApp) << "session log stopped:" << filePath << bytesWritten << "bytes";
        m_autoLogPort.clear();   // any stop, from any path, ends the auto-log association
        emit loggingChanged(false, filePath);
        emit statusMessage(tr("Log closed: %1 (%2 bytes)").arg(QDir::toNativeSeparators(filePath)).arg(bytesWritten),
                           kStatusShortMs);
    });
    connect(m_logger, &SessionLogger::error, this, [this](const QString& message) {
        qCWarning(lcApp) << "session log error:" << message;
        emit loggingChanged(m_logger->isActive(), m_logger->filePath());
        emit statusMessage(message, kStatusLongMs);
    });

    // ---- Preferences ----------------------------------------------------------------------
    connect(&AppSettings::instance(), &AppSettings::changed, this, [this](const QString&) { applyPreferences(); });

    onConnectionStateChanged(m_connection->state());
    applyPreferences();
}

SessionWidget::~SessionWidget()
{
    // The widget is going away: stop forwarding state/log signals to a half-destroyed owner.
    disconnect(m_connection, nullptr, this, nullptr);
    disconnect(m_logger, nullptr, this, nullptr);
    if (m_replayer) {
        disconnect(m_replayer, nullptr, this, nullptr);
    }
    if (m_logger->isActive()) {
        m_logger->stop();
    }
    m_connection->close();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void SessionWidget::setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_bar = new ConnectionBar(this);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("viewStack"));
    m_terminal = new TerminalWidget(m_stack);
    m_hexView = new HexDumpView(m_stack);
    m_stack->addWidget(m_terminal);
    m_stack->addWidget(m_hexView);
    m_stack->setCurrentWidget(m_terminal);

    m_quickBar = new QuickCommandBar(m_quickCommands, this);

    m_input = new CommandInput(this);
    m_input->setHistory(m_history);

    layout->addWidget(m_bar);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_quickBar);
    layout->addWidget(m_input);

    setFocusProxy(m_terminal);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

SerialConnection* SessionWidget::connection() const
{
    return m_connection;
}

TerminalWidget* SessionWidget::terminal() const
{
    return m_terminal;
}

HexDumpView* SessionWidget::hexView() const
{
    return m_hexView;
}

ConnectionBar* SessionWidget::connectionBar() const
{
    return m_bar;
}

CommandInput* SessionWidget::commandInput() const
{
    return m_input;
}

QuickCommandBar* SessionWidget::quickCommandBar() const
{
    return m_quickBar;
}

SessionLogger* SessionWidget::logger() const
{
    return m_logger;
}

QString SessionWidget::title() const
{
    if (isReplaying()) {
        return tr("Replay: %1").arg(QFileInfo(m_replayer->options().filePath).fileName());
    }
    const QString port = portName();
    return port.isEmpty() ? tr("New Session") : port;
}

bool SessionWidget::isConnected() const
{
    return m_connection->isOpen();
}

SessionWidget::ViewMode SessionWidget::viewMode() const
{
    return m_viewMode;
}

void SessionWidget::setViewMode(ViewMode mode)
{
    if (mode == m_viewMode) {
        return;
    }
    m_viewMode = mode;
    if (mode == ViewMode::HexDump) {
        m_stack->setCurrentWidget(m_hexView);
        setFocusProxy(m_hexView);
    } else {
        m_stack->setCurrentWidget(m_terminal);
        setFocusProxy(m_terminal);
    }
    emit viewModeChanged(mode);
}

bool SessionWidget::isLogging() const
{
    return m_logger->isActive();
}

QString SessionWidget::logFilePath() const
{
    return m_logger->filePath();
}

void SessionWidget::setPortName(const QString& portName)
{
    m_bar->selectPort(portName);
    SerialSettings settings = m_connection->settings();
    settings.portName = m_bar->selectedPortName();
    m_connection->setSettings(settings);
    m_bar->setConnectionState(m_connection->state()); // refresh the connect button dot
    emit titleChanged(title());
}

QString SessionWidget::portName() const
{
    return m_bar->selectedPortName();
}

// ---------------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------------

void SessionWidget::applyPreferences()
{
    const AppSettings& s = AppSettings::instance();

    // The font is pushed only when the preference itself changed, so a Ctrl+wheel zoom
    // survives unrelated settings writes (e.g. the last port name saved on connect).
    const QFont font = s.terminalFont();
    if (!m_preferencesApplied || font != m_appliedFont) {
        m_terminal->setTerminalFont(font);
        m_hexView->setFont(font);
        m_appliedFont = font;
    }
    m_terminal->setColorPalette(TerminalTheme::palette(s.themeName()));
    m_terminal->setScrollbackMax(s.scrollbackLines());
    m_terminal->setCursorBlink(s.cursorBlink());
    m_terminal->setBellEnabled(s.bellEnabled());
    m_terminal->setEnterSends(s.enterSends());
    m_terminal->setBackspaceSendsDelete(s.backspaceSendsDelete());
    m_terminal->setLocalEcho(s.localEcho());
    // Changing the encoding resets the stateful decoder, so only do it when it differs.
    if (m_terminal->encoding().compare(s.encoding(), Qt::CaseInsensitive) != 0) {
        if (!m_terminal->setEncoding(s.encoding())) {
            qCWarning(lcUi) << "unknown encoding" << s.encoding() << "- terminal falls back to UTF-8";
        }
    }
    m_terminal->setImplicitCr(s.implicitCr());

    m_connection->setAutoReconnect(s.autoReconnect());
    m_connection->setReconnectIntervalMs(s.reconnectIntervalMs());

    if (!m_preferencesApplied) {
        // Seed the line-mode input once; afterwards the combo belongs to the user.
        m_input->setLineEnding(s.enterSends());
        m_preferencesApplied = true;
    }
}

// ---------------------------------------------------------------------------
// Connection control
// ---------------------------------------------------------------------------

bool SessionWidget::connectPort()
{
    if (m_connection->isOpen()) {
        return true;
    }

    const SerialSettings settings = m_bar->settings();
    if (settings.portName.isEmpty()) {
        emit statusMessage(tr("Select a serial port first"), kStatusShortMs);
        m_bar->setFocusToPort();
        return false;
    }

    if (isReplaying()) {
        // The replay streams into the same terminal/hex view/logger as live data; opening the
        // port would interleave the two (the mirror of the refusal in replayLogFile()).
        qCInfo(lcApp) << "stopping replay before opening" << settings.portName;
        // Synchronous: emits finished(false) -> "replay of <file> stopped" line,
        // replayStateChanged(false), titleChanged(); isReplaying() is false before open().
        stopReplay();
    }

    m_connection->setSettings(settings);
    qCInfo(lcSerial) << "opening" << settings.portName << settings.summary();
    const bool ok = m_connection->open();
    if (!ok) {
        // The connection already reported the reason through errorOccurred().
        return false;
    }

    AppSettings::instance().setLastPortName(settings.portName);
    emit statusMessage(tr("Connected to %1 (%2)").arg(settings.portName, settings.summary()), kStatusShortMs);

    if (AppSettings::instance().autoLog()) {
        // A log the user started by hand is kept; an auto-started log for a *different* port is
        // closed so the new session gets its own "<port>_<time>.log" with a matching header.
        const bool autoLogForOtherPort =
            m_logger->isActive() && !m_autoLogPort.isEmpty() && m_autoLogPort != settings.portName;
        if (!m_logger->isActive() || autoLogForOtherPort) {
            // startLoggingTo() stops the old log first (stopped -> m_autoLogPort cleared), so the
            // assignment below re-establishes the association for the new file.
            startLoggingTo(SessionLogger::suggestFileName(settings.portName, AppSettings::instance().logDirectory()));
            if (m_logger->isActive()) {
                m_autoLogPort = settings.portName;
            }
        }
    }

    focusTerminal();
    return true;
}

void SessionWidget::disconnectPort()
{
    const SerialConnection::State before = m_connection->state();
    const qint64 dropped = m_connection->pendingTxBytes();
    m_connection->close();
    if (before != SerialConnection::State::Disconnected) {
        qCInfo(lcSerial) << "closed" << m_connection->portName();
        if (dropped > 0) {
            writeSystemLine(tr("%1 unsent bytes discarded on disconnect").arg(dropped));
            emit statusMessage(tr("Disconnected from %1 (%2 unsent bytes discarded)").arg(portName()).arg(dropped),
                               kStatusLongMs);
        } else {
            emit statusMessage(tr("Disconnected from %1").arg(m_connection->portName()), kStatusShortMs);
        }
    }
}

void SessionWidget::toggleConnection()
{
    const SerialConnection::State state = m_connection->state();
    if (state == SerialConnection::State::Connected || state == SerialConnection::State::Reconnecting) {
        disconnectPort();
    } else {
        connectPort();
    }
}

void SessionWidget::clearTerminal()
{
    m_terminal->clearScreen();
    m_hexView->clearAll();
}

void SessionWidget::resetTerminal()
{
    m_terminal->resetTerminal();
    emit statusMessage(tr("Terminal reset"), kStatusShortMs);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void SessionWidget::startLogging()
{
    const QString port = portName().isEmpty() ? QStringLiteral("session") : portName();
    const QString suggested = SessionLogger::suggestFileName(port, AppSettings::instance().logDirectory());
    const QString path = QFileDialog::getSaveFileName(this, tr("Save session log"), suggested,
                                                      tr("Log files (*.log *.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    startLoggingTo(path);
}

void SessionWidget::startLoggingTo(const QString& filePath)
{
    if (filePath.isEmpty()) {
        return;
    }
    if (m_logger->isActive()) {
        if (m_logger->filePath() == filePath) {
            return;
        }
        m_logger->stop();
    }

    const AppSettings& s = AppSettings::instance();
    const SessionLogger::Format format = SessionLogger::formatFromString(s.logFormat());
    // SessionLogger wraps this as "# BuildAI Serial Utility log - <port> <settings> - started <time>".
    const QString port = portName().isEmpty() ? tr("(no port)") : portName();
    const QString header = QStringLiteral("%1 %2").arg(port, m_connection->settings().summary());

    if (!m_logger->start(filePath, format, s.logIncludeTx(), header)) {
        // error() was emitted by the logger and is already shown in the status bar.
        qCWarning(lcApp) << "could not start session log" << filePath;
    }
}

void SessionWidget::stopLogging()
{
    if (m_logger->isActive()) {
        m_logger->stop();
    }
}

void SessionWidget::toggleLogging()
{
    if (m_logger->isActive()) {
        stopLogging();
    } else {
        startLogging();
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void SessionWidget::sendFile(const QString& path)
{
    if (!m_sendFileDialog) {
        m_sendFileDialog = new SendFileDialog(this);
        m_sendFileDialog->setModal(false);
        // Backpressure: the dialog learns how much of what it queued is still in the port's
        // write buffer, after every chunk and whenever the driver drains some of it.
        connect(m_sendFileDialog, &SendFileDialog::sendChunk, this, [this](const QByteArray& chunk) {
            sendBytes(chunk);
            m_sendFileDialog->updatePendingTx(m_connection->pendingTxBytes());
        });
        connect(m_connection, &SerialConnection::txBytesWritten, m_sendFileDialog,
                [this](qint64) { m_sendFileDialog->updatePendingTx(m_connection->pendingTxBytes()); });
        connect(m_sendFileDialog, &SendFileDialog::sendingStarted, this, [this]() {
            emit statusMessage(tr("Sending %1...").arg(QFileInfo(m_sendFileDialog->filePath()).fileName()),
                               kStatusShortMs);
        });
        connect(m_sendFileDialog, &SendFileDialog::sendingFinished, this,
                [this](bool completed, const QString& message) {
                    qCInfo(lcApp) << "file send finished:" << completed << message;
                    if (!message.isEmpty()) {
                        emit statusMessage(message, kStatusLongMs);
                    }
                });
    }
    m_sendFileDialog->setConnected(isConnected());
    if (!path.isEmpty()) {
        m_sendFileDialog->setFilePath(path);
    }
    m_sendFileDialog->show();
    m_sendFileDialog->raise();
    m_sendFileDialog->activateWindow();
}

void SessionWidget::sendBreak()
{
    if (!m_connection->isOpen()) {
        emit statusMessage(tr("Not connected"), kStatusShortMs);
        return;
    }
    if (!m_connection->sendBreak()) {
        // The connection already reported the reason through errorOccurred() -> onConnectionError().
        return;
    }
    qCInfo(lcSerial) << "BREAK sent on" << m_connection->portName();
    emit statusMessage(tr("BREAK sent"), kStatusShortMs);
}

void SessionWidget::syncTerminalSize()
{
    const int cols = m_terminal->columns();
    const int rows = m_terminal->visibleRows();
    if (cols <= 0 || rows <= 0) {
        return;
    }
    sendBytes(QStringLiteral("stty cols %1 rows %2\r").arg(cols).arg(rows).toLatin1());
}

void SessionWidget::sendBytes(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    if (!m_connection->isOpen()) {
        emit statusMessage(tr("Not connected"), kStatusShortMs);
        return;
    }
    if (m_connection->write(bytes) < 0) {
        emit statusMessage(tr("Write to %1 failed").arg(m_connection->portName()), kStatusLongMs);
    }
}

void SessionWidget::sendQuickCommand(const QuickCommand& command)
{
    QString error;
    const QByteArray payload = command.payload(&error);
    if (!error.isEmpty()) {
        const QString name = command.name.isEmpty() ? command.command : command.name;
        emit statusMessage(tr("Quick command \"%1\": %2").arg(name, error), kStatusLongMs);
        qCWarning(lcUi) << "quick command" << name << "has an invalid payload:" << error;
        return;
    }
    if (command.hex) {
        sendBytes(payload);   // hex payloads are never transcoded
    } else if (command.escapes) {
        // Re-run the unescape with the session encoding so text runs are transcoded while
        // \xHH stays the exact byte; payload() already validated the same text.
        QString unescapeError;
        QByteArray bytes = unescapeForDevice(command.command, &unescapeError);
        if (!unescapeError.isEmpty()) {
            bytes = encodeForDevice(payload);
        } else {
            bytes += LineEnding::bytes(command.lineEnding);
        }
        sendBytes(bytes);
    } else {
        sendBytes(encodeForDevice(payload));
    }
}

void SessionWidget::focusTerminal()
{
    if (m_viewMode == ViewMode::HexDump) {
        m_hexView->setFocus(Qt::OtherFocusReason);
    } else {
        m_terminal->setFocus(Qt::OtherFocusReason);
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void SessionWidget::onConnectionStateChanged(SerialConnection::State state)
{
    const bool connected = (state == SerialConnection::State::Connected);

    m_bar->setConnectionState(state);
    if (connected) {
        m_bar->setPinStates(m_connection->dtr(), m_connection->rts());
    }
    m_terminal->setInputEnabled(connected);
    m_input->setEnabledForConnection(connected);
    m_quickBar->setEnabledForConnection(connected);
    if (m_sendFileDialog) {
        m_sendFileDialog->setConnected(connected);
    }

    emit connectionStateChanged(state);
    emit titleChanged(title());
}

void SessionWidget::onConnectionError(const QString& message)
{
    qCWarning(lcSerial) << message;
    emit statusMessage(message, kStatusLongMs);
}

void SessionWidget::onPortDisappeared(const QString& portName)
{
    // Info only: the connection layer already logged the warning with the underlying error.
    qCInfo(lcSerial) << "port" << portName << "disappeared";
    if (m_connection->autoReconnect()) {
        writeSystemLine(tr("port %1 disappeared, waiting to reconnect").arg(portName));
        emit statusMessage(tr("Port %1 disappeared - waiting for it to come back").arg(portName), kStatusLongMs);
    } else {
        writeSystemLine(tr("port %1 disappeared").arg(portName));
        emit statusMessage(tr("Port %1 disappeared").arg(portName), kStatusLongMs);
    }
}

void SessionWidget::onReconnected(const QString& portName)
{
    qCInfo(lcSerial) << "reconnected to" << portName;
    writeSystemLine(tr("reconnected"));
    emit statusMessage(tr("Reconnected to %1").arg(portName), kStatusShortMs);
}

void SessionWidget::onBarSettingsChanged(const SerialSettings& settings)
{
    m_connection->setSettings(settings); // applied live while open (port name only at next open)
    const SerialSettings accepted = m_connection->settings();
    if (accepted != settings) {
        m_bar->setSettings(accepted);   // a driver-rejected value snaps the combo back; emits nothing
    }
    emit titleChanged(title());
}

void SessionWidget::onPortsChanged(const QList<SerialPortEntry>& ports)
{
    const QString before = title();
    m_bar->setPorts(ports);
    if (title() != before) {
        emit titleChanged(title());
    }
}

void SessionWidget::onInputSendRequested(const QByteArray& payload, const QString& displayText)
{
    qCDebug(lcUi) << "command input:" << displayText;
    if (m_input->hexMode()) {
        sendBytes(payload);   // hex payloads are never transcoded
    } else if (m_input->escapeMode()) {
        // Re-run the unescape on the typed text with the session encoding: text runs are
        // transcoded, \xHH stays the exact byte (payload flattened both into one byte array).
        QString error;
        QByteArray bytes = unescapeForDevice(displayText, &error);
        if (!error.isEmpty()) {
            bytes = encodeForDevice(payload);   // cannot happen: CommandInput validated the same text
        } else {
            bytes += LineEnding::bytes(m_input->lineEnding());
        }
        sendBytes(bytes);
    } else {
        sendBytes(encodeForDevice(payload));
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SessionWidget::writeSystemLine(const QString& text)
{
    // Dim (SGR 2) on its own line, encoded like device output so the parser decodes it correctly.
    QByteArray line = QByteArrayLiteral("\r\n\x1b[2m--- ");
    line += encodeForDevice(text.toUtf8());
    line += QByteArrayLiteral(" ---\x1b[0m\r\n");
    m_terminal->parser()->feed(line);
}

QByteArray SessionWidget::encodeForDevice(const QByteArray& utf8) const
{
    const QString encoding = m_terminal->encoding();
    if (encoding.isEmpty() || encoding.compare(QStringLiteral("UTF-8"), Qt::CaseInsensitive) == 0) {
        return utf8;
    }

    QStringEncoder encoder(encoding.toUtf8().constData());
    if (!encoder.isValid()) {
        qCWarning(lcUi) << "no encoder for" << encoding << "- sending UTF-8";
        return utf8;
    }
    QStringDecoder decoder(QStringConverter::Utf8);
    const QString text = decoder.decode(utf8);
    return encoder.encode(text);
}

QByteArray SessionWidget::unescapeForDevice(const QString& text, QString* error) const
{
    const QString encoding = m_terminal->encoding();
    const bool utf8 = encoding.isEmpty() || encoding.compare(QStringLiteral("UTF-8"), Qt::CaseInsensitive) == 0 ||
                      !QStringEncoder(encoding.toUtf8().constData()).isValid();
    if (utf8) {
        return HexUtils::unescape(
            text, [](const QString& s) { return s.toUtf8(); }, error);
    }
    // A fresh encoder per text run: runs are independent (a \xHH byte may sit between them),
    // so no shift state must carry over.
    return HexUtils::unescape(
        text,
        [&encoding](const QString& s) {
            QStringEncoder encoder(encoding.toUtf8().constData());
            return QByteArray(encoder.encode(s));
        },
        error);
}

// ---------------------------------------------------------------------------
// Log replay (core/LogReplayer.h)
// ---------------------------------------------------------------------------

bool SessionWidget::isReplaying() const
{
    return m_replayer && m_replayer->isRunning();
}

void SessionWidget::replayLogFile(const QString& path, qint64 bytesPerSecond)
{
    if (m_connection->state() != SerialConnection::State::Disconnected) {
        // Replayed bytes would interleave with live device output; the user disconnects first.
        emit statusMessage(tr("Disconnect from %1 before replaying a log file").arg(m_connection->portName()),
                           kStatusLongMs);
        return;
    }

    QString file = path;
    if (file.isEmpty()) {
        file = QFileDialog::getOpenFileName(this, tr("Replay Log File"), AppSettings::instance().logDirectory(),
                                            tr("Log files (*.log *.txt);;All files (*)"));
        if (file.isEmpty()) {
            return;
        }
    }
    qint64 speed = bytesPerSecond;
    if (speed < 0 && !askReplaySpeed(speed)) {
        return;
    }

    if (!m_replayer) {
        m_replayer = new LogReplayer(this);
        // Replayed bytes take exactly the path of received data: terminal, hex view, logger.
        connect(m_replayer, &LogReplayer::chunkReady, this, [this](const QByteArray& bytes) {
            m_terminal->feedData(bytes);
            m_hexView->appendReceived(bytes);
            m_logger->logReceived(bytes);
        });
        connect(m_replayer, &LogReplayer::finished, this, [this](bool completed) {
            const QString name = QFileInfo(m_replayer->options().filePath).fileName();
            qCInfo(lcApp) << "replay of" << name << (completed ? "finished" : "stopped");
            writeSystemLine(completed ? tr("replay of %1 finished").arg(name) : tr("replay of %1 stopped").arg(name));
            emit statusMessage(completed ? tr("Replay of %1 finished").arg(name) : tr("Replay of %1 stopped").arg(name),
                               kStatusShortMs);
            emit replayStateChanged(false);
            emit titleChanged(title());   // back to the port name / "New Session"
        });
    }
    if (m_replayer->isRunning()) {
        m_replayer->stop();
    }

    LogReplayer::Options options;
    options.filePath = file;
    options.bytesPerSecond = speed;
    options.autoDetectFormat = true;
    if (!m_replayer->start(options)) {
        qCWarning(lcApp) << "replay failed:" << m_replayer->lastError();
        emit statusMessage(m_replayer->lastError(), kStatusLongMs);
        return;
    }
    writeSystemLine(tr("replaying %1").arg(QDir::toNativeSeparators(file)));
    emit statusMessage(tr("Replaying %1...").arg(QFileInfo(file).fileName()), kStatusShortMs);
    emit replayStateChanged(true);
    emit titleChanged(title());   // the tab shows the replayed file while it streams
    focusTerminal();
}

void SessionWidget::stopReplay()
{
    if (isReplaying()) {
        m_replayer->stop();   // emits finished(false) -> status line + replayStateChanged(false)
    }
}

bool SessionWidget::askReplaySpeed(qint64& bytesPerSecond)
{
    const QList<QPair<QString, qint64>> speeds = LogReplayer::standardSpeeds();
    QStringList labels;
    int defaultIndex = 0;
    for (qsizetype i = 0; i < speeds.size(); ++i) {
        labels.append(speeds.at(i).first);
        if (speeds.at(i).second == 11520) {   // 115200 baud
            defaultIndex = static_cast<int>(i);
        }
    }
    bool ok = false;
    const QString chosen =
        QInputDialog::getItem(this, tr("Replay Speed"), tr("Replay the file at:"), labels, defaultIndex, false, &ok);
    if (!ok) {
        return false;
    }
    const qsizetype index = labels.indexOf(chosen);
    bytesPerSecond = index >= 0 ? speeds.at(index).second : 11520;
    return true;
}
