#pragma once

#include <QWidget>
#include <QString>
#include <QFont>

#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"
#include "core/QuickCommand.h"

class QStackedWidget;
class QLabel;
class TerminalWidget;
class HexDumpView;
class ConnectionBar;
class CommandInput;
class QuickCommandBar;
class QuickCommandStore;
class CommandHistory;
class SessionLogger;
class SendFileDialog;
class LogReplayer;

/**
 * One tab of the main window = one serial port session.
 *
 *  ┌ ConnectionBar ──────────────────────────────────────────────┐
 *  ├ QStackedWidget { TerminalWidget | HexDumpView } ────────────┤
 *  ├ QuickCommandBar ────────────────────────────────────────────┤
 *  └ CommandInput ───────────────────────────────────────────────┘
 *
 * Wiring (all in the constructor):
 *  - connection.dataReceived  -> terminal.feedData, hexView.appendReceived, logger.logReceived
 *  - connection.dataSent      -> hexView.appendSent, logger.logSent
 *  - connection.stateChanged  -> bar.setConnectionState, terminal.setInputEnabled(connected),
 *                                input/quick bar enable, titleChanged(), connectionStateChanged()
 *  - connection.errorOccurred -> statusMessage(msg, 5000) + qCWarning(lcSerial)
 *  - connection.portDisappeared / reconnected -> status messages; the terminal shows a
 *    dim system line "--- port COM8 disappeared, waiting to reconnect ---" / "--- reconnected ---"
 *    (fed through terminal()->feedData() with SGR dim so it is visually distinct and queues like
 *    device output while the display is paused for a selection).
 *  - terminal.outputPausedChanged -> statusMessage(persistentStatusMessage(), 0): "Output paused
 *    while selecting - Enter copies, Esc cancels" on pause, an empty string on resume (MainWindow
 *    clears the status bar and re-shows the hint after a transient message / on tab change);
 *    terminal.pauseBufferOverflow -> a 5 s status message with the flushed size.
 *  - terminal.sendData        -> sendBytes()
 *  - input.sendRequested      -> sendBytes(payload) (HEX: verbatim; Esc: text runs transcoded to the
 *                                session encoding, \xHH bytes verbatim; plain: transcoded when not UTF-8)
 *  - quickBar.commandTriggered-> sendBytes(command.payload()) with the same HEX / escapes / plain
 *                                rules as the command input (payload error -> statusMessage)
 *  - bar.connectRequested/disconnectRequested/settingsChanged/dtr/rts/break -> connection
 *  - bar.refreshRequested     -> SerialPortEnumerator::instance().refresh()
 *  - SerialPortEnumerator.portsChanged -> bar.setPorts
 *  - terminal.fileDropped     -> sendFile(path)
 *  - terminal.syncSizeRequested -> syncTerminalSize(); terminal.gridSizeChanged -> gridSizeChanged();
 *    terminal.findRequested -> findRequested()
 *  - terminal.titleChanged    -> statusMessage(title, 3000) (OSC titles from the device)
 *  - connection.countersChanged -> countersChanged()
 *  - logger.started/stopped/error -> loggingChanged() + statusMessage()
 *  - AppSettings.changed      -> applyPreferences()
 *
 * sendBytes() is the single TX path: it calls connection()->write(); when the port is not
 * open it emits statusMessage("Not connected") and drops the bytes.
 *
 * Title: portName when a port is selected (plus " *" while connected is NOT used; the tab
 * icon/colour conveys state instead - MainWindow reads isConnected()). "New Session" when
 * no port is selected. While a log replay runs the title is "Replay: <file name>" and
 * titleChanged() is emitted when the replay starts and when it ends.
 *
 * Auto-log: when AppSettings::autoLog() is on, connectPort() starts a SessionLogger with
 * SessionLogger::suggestFileName(port, AppSettings::logDirectory()) using logFormat()/logIncludeTx()
 * unless a log is already active; an auto-started log for a different port is closed and
 * replaced, a log started by the user (startLogging()/startLoggingTo()) is kept.
 */
class SessionWidget : public QWidget
{
    Q_OBJECT
public:
    enum class ViewMode { Terminal, HexDump };
    Q_ENUM(ViewMode)

    SessionWidget(QuickCommandStore* quickCommands, CommandHistory* history, QWidget* parent = nullptr);
    ~SessionWidget() override;

    SerialConnection* connection() const;
    TerminalWidget* terminal() const;
    HexDumpView* hexView() const;
    ConnectionBar* connectionBar() const;
    CommandInput* commandInput() const;
    QuickCommandBar* quickCommandBar() const;
    SessionLogger* logger() const;

    QString title() const;
    bool isConnected() const;
    ViewMode viewMode() const;
    void setViewMode(ViewMode mode);
    bool isLogging() const;
    QString logFilePath() const;
    /// True while a LogReplayer streams a captured file into this session (see replayLogFile()).
    /// Public getter added at integration together with the replay slots below.
    bool isReplaying() const;

    /// The status-bar text for a lasting state of this session - currently the terminal paused
    /// while selecting ("Output paused while selecting - Enter copies, Esc cancels") - or an empty
    /// string. statusMessage(text, 0) announces every transition; MainWindow re-reads this when
    /// the tab becomes current and once a transient message expired, so the hint never outlives a
    /// tab switch or a close and is not lost behind a 3 s message.
    QString persistentStatusMessage() const;

    /// Pre-select a port (used when restoring sessions / "New Session" with a port).
    void setPortName(const QString& portName);
    QString portName() const;

    /// Re-read AppSettings and push font/theme/scrollback/enter/backspace/echo/encoding/
    /// implicitCr/bell/cursorBlink/pauseWhileSelecting/rightClickPastes to the terminal and
    /// autoReconnect/interval to the connection.
    void applyPreferences();

public slots:
    /// Open the port selected in the ConnectionBar with its settings; returns success (true when
    /// already open). A running log replay is stopped first (its usual "stopped" status line is
    /// emitted) so replayed and live bytes never interleave; see replayLogFile().
    bool connectPort();
    void disconnectPort();
    void toggleConnection();
    /// Toolbar / Session > Clear (Ctrl+Shift+L): terminal()->clearAll() - screen *and* scrollback
    /// gone, cursor home, attributes and modes kept - and hexView()->clearAll(); a paused display
    /// is cleared first and its queued bytes flushed afterwards. resetTerminal() is the full RIS.
    void clearTerminal();
    void resetTerminal();
    void startLogging();           ///< QFileDialog::getSaveFileName seeded with suggestFileName(); uses AppSettings format
    void startLoggingTo(const QString& filePath);
    void stopLogging();
    void toggleLogging();
    void sendFile(const QString& path = QString());   ///< opens SendFileDialog (modeless, parented to this)
    /// statusMessage("BREAK sent") only when the connection asserted BREAK; failures surface via
    /// errorOccurred -> statusMessage.
    void sendBreak();
    /// Sends "stty cols <cols> rows <rows>\r" so a Linux shell over UART matches the widget.
    void syncTerminalSize();
    void sendBytes(const QByteArray& bytes);
    void sendQuickCommand(const QuickCommand& command);
    void focusTerminal();
    /// Replay a captured log file (raw or SessionLogger text capture) through the terminal,
    /// hex view and logger as if the device sent it (core/LogReplayer.h). An empty `path`
    /// opens a file dialog; `bytesPerSecond` < 0 asks the user for the speed (LogReplayer::
    /// standardSpeeds()), 0 = unlimited. Only while disconnected: when the connection is
    /// Connected or Reconnecting the request is refused with a statusMessage() (replayed
    /// bytes would interleave with live device output).
    void replayLogFile(const QString& path = QString(), qint64 bytesPerSecond = -1);
    void stopReplay();

signals:
    void titleChanged(const QString& title);
    void connectionStateChanged(SerialConnection::State state);
    void statusMessage(const QString& message, int timeoutMs);
    void countersChanged(quint64 rx, quint64 tx);
    void loggingChanged(bool active, const QString& filePath);
    void viewModeChanged(SessionWidget::ViewMode mode);
    void gridSizeChanged(int rows, int cols);
    void quickCommandsEditRequested();
    void findRequested();   ///< terminal context menu "Find..."
    /// A log replay started (true) or finished/stopped (false); MainWindow refreshes its actions.
    void replayStateChanged(bool active);

private slots:
    void onConnectionStateChanged(SerialConnection::State state);
    void onConnectionError(const QString& message);
    void onPortDisappeared(const QString& portName);
    void onReconnected(const QString& portName);
    void onBarSettingsChanged(const SerialSettings& settings);
    void onPortsChanged(const QList<SerialPortEntry>& ports);
    void onInputSendRequested(const QByteArray& payload, const QString& displayText);

private:
    void setupUi();
    void writeSystemLine(const QString& text);   ///< dim informational line in the terminal
    /// Plain-text payloads only (UTF-8 in): transcoded to the session encoding when it is not
    /// UTF-8. Never use it on bytes that may contain \xHH escapes; see unescapeForDevice().
    QByteArray encodeForDevice(const QByteArray& utf8) const;
    /// HexUtils::unescape(text, encodeText, error) with encodeText = the session encoding
    /// (UTF-8 when the encoding is empty, "UTF-8" or has no encoder): text runs are transcoded,
    /// \xHH bytes are appended verbatim. No line ending is added.
    QByteArray unescapeForDevice(const QString& text, QString* error) const;
    bool askReplaySpeed(qint64& bytesPerSecond);                 ///< QInputDialog over LogReplayer::standardSpeeds()

    SerialConnection* m_connection = nullptr;
    ConnectionBar* m_bar = nullptr;
    QStackedWidget* m_stack = nullptr;
    TerminalWidget* m_terminal = nullptr;
    HexDumpView* m_hexView = nullptr;
    QuickCommandBar* m_quickBar = nullptr;
    CommandInput* m_input = nullptr;
    SessionLogger* m_logger = nullptr;
    QuickCommandStore* m_quickCommands = nullptr;
    CommandHistory* m_history = nullptr;
    SendFileDialog* m_sendFileDialog = nullptr;
    LogReplayer* m_replayer = nullptr;       ///< created on the first replayLogFile(); QObject child
    ViewMode m_viewMode = ViewMode::Terminal;
    /// Port the active log was auto-started for; empty when the log was started by the user or
    /// no log is active.
    QString m_autoLogPort;
    bool m_preferencesApplied = false;   ///< first applyPreferences() also seeds the command input line ending
    QFont m_appliedFont;                 ///< terminal font last pushed by applyPreferences() (keeps user zoom otherwise)
};
