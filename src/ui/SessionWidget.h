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
 *    (written via terminal()->parser()->feed() with SGR dim so it is visually distinct).
 *  - terminal.sendData        -> sendBytes()
 *  - input.sendRequested      -> sendBytes(payload)   (encoding applied when not UTF-8)
 *  - quickBar.commandTriggered-> sendBytes(command.payload()) (error -> statusMessage)
 *  - bar.connectRequested/disconnectRequested/settingsChanged/dtr/rts/break -> connection
 *  - bar.refreshRequested     -> SerialPortEnumerator::instance().refresh()
 *  - SerialPortEnumerator.portsChanged -> bar.setPorts
 *  - terminal.fileDropped     -> sendFile(path)
 *  - terminal.syncSizeRequested -> syncTerminalSize(); terminal.gridSizeChanged -> gridSizeChanged()
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
 * no port is selected.
 *
 * Auto-log: when AppSettings::autoLog() is on, connectPort() starts a SessionLogger with
 * SessionLogger::suggestFileName(port, AppSettings::logDirectory()) using logFormat()/logIncludeTx().
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

    /// Pre-select a port (used when restoring sessions / "New Session" with a port).
    void setPortName(const QString& portName);
    QString portName() const;

    /// Re-read AppSettings and push font/theme/scrollback/enter/backspace/echo/encoding/
    /// implicitCr/bell/cursorBlink to the terminal and autoReconnect/interval to the connection.
    void applyPreferences();

public slots:
    bool connectPort();
    void disconnectPort();
    void toggleConnection();
    void clearTerminal();          ///< terminal()->clearScreen() and hexView()->clearAll()
    void resetTerminal();
    void startLogging();           ///< QFileDialog::getSaveFileName seeded with suggestFileName(); uses AppSettings format
    void startLoggingTo(const QString& filePath);
    void stopLogging();
    void toggleLogging();
    void sendFile(const QString& path = QString());   ///< opens SendFileDialog (modeless, parented to this)
    void sendBreak();
    /// Sends "stty cols <cols> rows <rows>\r" so a Linux shell over UART matches the widget.
    void syncTerminalSize();
    void sendBytes(const QByteArray& bytes);
    void sendQuickCommand(const QuickCommand& command);
    void focusTerminal();
    /// Replay a captured log file (raw or SessionLogger text capture) through the terminal,
    /// hex view and logger as if the device sent it (core/LogReplayer.h). An empty `path`
    /// opens a file dialog; `bytesPerSecond` < 0 asks the user for the speed (LogReplayer::
    /// standardSpeeds()), 0 = unlimited. Works while disconnected (it is RX only).
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
    QByteArray encodeForDevice(const QByteArray& utf8) const;   ///< transcode when encoding != UTF-8
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
    bool m_preferencesApplied = false;   ///< first applyPreferences() also seeds the command input line ending
    QFont m_appliedFont;                 ///< terminal font last pushed by applyPreferences() (keeps user zoom otherwise)
};
