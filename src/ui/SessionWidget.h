#pragma once

#include <QWidget>
#include <QString>
#include <QFont>

#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"
#include "core/QuickCommand.h"
#include "core/Transport.h"
#include "ssh/SshConnection.h"

class QStackedWidget;
class QLabel;
class TerminalWidget;
class HexDumpView;
class ConnectionBar;
class SshConnectionBar;
class SshProfileStore;
class CommandInput;
class QuickCommandBar;
class QuickCommandStore;
class CommandHistory;
class SessionLogger;
class SendFileDialog;
class RemoteFileDialog;
class LogReplayer;

/**
 * One tab of the main window = one session over a Transport: a serial port (Kind::Serial,
 * SerialConnection + ConnectionBar) or an SSH shell (Kind::Ssh, SshConnection + SshConnectionBar).
 * The kind is fixed at construction; the two-argument constructor means Kind::Serial.
 *
 *  ┌ ConnectionBar | SshConnectionBar ────────────────────────────┐
 *  ├ QStackedWidget { TerminalWidget | HexDumpView } ────────────┤
 *  ├ QuickCommandBar ────────────────────────────────────────────┤
 *  └ CommandInput ───────────────────────────────────────────────┘
 *
 * Wiring shared by both kinds (all in the constructor, only through transport()):
 *  - transport.dataReceived  -> terminal.feedData, hexView.appendReceived, logger.logReceived
 *  - transport.dataSent      -> hexView.appendSent, logger.logSent
 *  - transport.stateChanged  -> bar.setConnectionState, terminal.setInputEnabled(connected),
 *                               input/quick bar enable, titleChanged(), connectionStateChanged()
 *  - transport.errorOccurred -> statusMessage(msg, 5000) + qCWarning(lcSerial / lcSsh)
 *  - transport.connectionLost / connectionRestored -> status messages; the terminal shows a
 *    dim system line "--- port COM8 disappeared, waiting to reconnect ---" / "--- reconnected ---"
 *    (SSH: "--- connection to root@host lost, waiting to reconnect ---"), fed through
 *    terminal()->feedData() with SGR dim so it is visually distinct and queues like device
 *    output while the display is paused for a selection.
 *  - transport.countersChanged -> countersChanged()
 *  - terminal.outputPausedChanged -> statusMessage(persistentStatusMessage(), 0): "Output paused
 *    while selecting - Enter copies, Esc cancels" on pause, an empty string on resume (MainWindow
 *    clears the status bar and re-shows the hint after a transient message / on tab change);
 *    terminal.pauseBufferOverflow -> a 5 s status message with the flushed size.
 *  - terminal.sendData        -> sendBytes()
 *  - terminal.gridSizeChanged -> gridSizeChanged() and transport.notifyTerminalSize(cols, rows)
 *                                (a window-change request for SSH; serial ignores it)
 *  - terminal.syncSizeRequested -> syncTerminalSize(); terminal.findRequested -> findRequested()
 *  - terminal.fileDropped     -> sendFile(path) (serial) / uploadFile(path) (SSH)
 *  - terminal.titleChanged    -> statusMessage(title, 3000) (OSC titles from the device)
 *  - input.sendRequested      -> sendBytes(payload) (HEX: verbatim; Esc: text runs transcoded to the
 *                                session encoding, \xHH bytes verbatim; plain: transcoded when not UTF-8)
 *  - quickBar.commandTriggered-> sendBytes(command.payload()) with the same HEX / escapes / plain
 *                                rules as the command input (payload error -> statusMessage)
 *  - logger.started/stopped/error -> loggingChanged() + statusMessage()
 *  - AppSettings.changed      -> applyPreferences() (terminal settings; autoReconnect / interval
 *                                are pushed to the transport of either kind)
 *
 * Serial only:
 *  - bar.connectRequested/disconnectRequested/settingsChanged/dtr/rts/break -> connection
 *  - bar.refreshRequested     -> SerialPortEnumerator::instance().refresh()
 *  - SerialPortEnumerator.portsChanged -> bar.setPorts
 *  - connection.pinsChanged   -> bar.setPinStates
 *
 * SSH only (sshConnection(), sshConnectionBar()->setStore(profiles)):
 *  - sshBar.connectRequested/disconnectRequested -> connectPort()/disconnectPort();
 *    sshBar.profilesEditRequested -> sshProfilesEditRequested(); sshBar.profileChanged ->
 *    the profile is pushed to the connection while disconnected, titleChanged(title())
 *  - ssh.hostKeyVerificationRequired -> HostKeyDialog (modal on window()) -> answerHostKey()
 *  - ssh.authPromptRequired   -> AuthPromptDialog -> answerPrompt() / cancelPrompt()
 *  - ssh.bannerReceived       -> every line as a dim line in the terminal
 *  - ssh.authenticated(method)-> statusMessage("Authenticated (method)")
 *  - ssh.shellStarted         -> system line "--- connected to user@host:port (ssh-ed25519,
 *                                publickey) ---", notifyTerminalSize(current grid)
 *  - transport.stateChanged(Connected) -> requestRemoteHome() (the facade accepts it only once
 *                                Connected, which it reports after shellStarted) and, when the
 *                                connect was started by the user (previous state Connecting, not
 *                                Reconnecting), the auto-log described below
 *  - ssh.channelClosed(status)-> system line "--- connection closed (exit status N) ---"
 *                                ("--- connection closed ---" when the status is unknown)
 *  - SshProfileStore::changed -> while disconnected the bar's current profile (renamed in the
 *                                profiles dialog, or the plain target the bar falls back to when
 *                                the profile was deleted) is pushed to the connection again, so
 *                                title() and restoreKey() follow the store
 *  - connectPort() takes the bar's current profile: a stored profile is touched in the store, an
 *    ad-hoc target goes to the store's recent list, AppSettings::setLastSshTarget(restoreKey()),
 *    then SshConnection::open() (asynchronous: Connecting -> Connected / Disconnected). The
 *    AppSettings SSH defaults (known_hosts file, and for ad-hoc targets the identity file,
 *    terminal type and keep-alive) fill the profile fields that are empty.
 *  - sendBreak() reports "Not available for SSH sessions"; syncTerminalSize() sends nothing
 *    through the shell but a window-change request; uploadFile()/downloadFile() open the
 *    session's RemoteFileDialog (modeless, one per session, refused while disconnected).
 *
 * sendBytes() is the single TX path: it calls transport()->write(); when the transport is not
 * open it emits statusMessage("Not connected") and drops the bytes.
 *
 * Title: transport()->displayName() - the port name, or the SSH profile's name / user@host:port
 * (plus " *" while connected is NOT used; the tab icon/colour conveys state instead - MainWindow
 * reads isConnected()). "New Session" (serial) / "New SSH Session" (SSH) when nothing is
 * selected. While a log replay runs the title is "Replay: <file name>" and titleChanged() is
 * emitted when the replay starts and when it ends.
 *
 * Session restore: portName() returns the serial port name or, for SSH, restoreKey()
 * ("ssh:profile:<id>" for a stored profile, "ssh:target:<user@host:port>" for an ad-hoc target,
 * empty without a target), so MainWindow persists one key per tab; setPortName() is ignored on
 * an SSH session and setSshTarget() applies a restore key or plain "user@host[:port]" text to
 * the SSH bar (never connecting).
 *
 * Auto-log: when AppSettings::autoLog() is on, connectPort() starts a SessionLogger with
 * SessionLogger::suggestFileName(name, AppSettings::logDirectory()) - name = the port, or the
 * SSH displayName() with every character outside [A-Za-z0-9._@-] replaced by "_" - using
 * logFormat()/logIncludeTx() unless a log is already active; an auto-started log for a different
 * target is closed and replaced, a log started by the user (startLogging()/startLoggingTo()) is kept.
 * A serial port opens synchronously, so its log starts inside connectPort(); an SSH session's log
 * starts when the state reaches Connected after a user-initiated connect, so the header carries
 * the host-key type and auth method and a failed or cancelled connect leaves no file behind.
 */
class SessionWidget : public QWidget
{
    Q_OBJECT
public:
    enum class ViewMode { Terminal, HexDump };
    Q_ENUM(ViewMode)

    /// A serial session (Kind::Serial).
    SessionWidget(QuickCommandStore* quickCommands, CommandHistory* history, QWidget* parent = nullptr);
    /// A session of the given kind; `profiles` (not owned, may be null) feeds the SSH bar.
    SessionWidget(Transport::Kind kind, QuickCommandStore* quickCommands, CommandHistory* history,
                  SshProfileStore* profiles, QWidget* parent = nullptr);
    ~SessionWidget() override;

    Transport* transport() const;            ///< always set
    SerialConnection* connection() const;    ///< serial only; nullptr for an SSH session
    SshConnection* sshConnection() const;    ///< SSH only; nullptr for a serial session
    TerminalWidget* terminal() const;
    HexDumpView* hexView() const;
    ConnectionBar* connectionBar() const;    ///< serial only; nullptr for an SSH session
    SshConnectionBar* sshConnectionBar() const;   ///< SSH only; nullptr for a serial session
    CommandInput* commandInput() const;
    QuickCommandBar* quickCommandBar() const;
    SessionLogger* logger() const;
    Transport::Kind kind() const;
    bool isSsh() const;

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

    /// Pre-select a port (used when restoring sessions / "New Session" with a port). Ignored on
    /// an SSH session (see setSshTarget()).
    void setPortName(const QString& portName);
    /// The selected serial port, or restoreKey() for an SSH session (what MainWindow persists).
    QString portName() const;

    /// Session-restore key: the port name for serial; "ssh:profile:<id>" (stored profile) or
    /// "ssh:target:<user@host:port>" (ad-hoc, SshProfile::displayTarget()) for SSH; empty when
    /// there is no target.
    QString restoreKey() const;
    /// True for the "ssh:profile:..." / "ssh:target:..." keys produced by restoreKey().
    static bool isSshRestoreKey(const QString& key);
    /// SSH only: apply a restore key or plain "user@host[:port]" text to the bar (selectProfile /
    /// setTarget) and to the connection's profile. Never connects. Ignored on a serial session.
    void setSshTarget(const QString& keyOrTarget);

    /// Re-read AppSettings and push font/theme/scrollback/enter/backspace/echo/encoding/
    /// implicitCr/bell/cursorBlink/pauseWhileSelecting/rightClickPastes to the terminal and
    /// autoReconnect/interval to the transport (both kinds).
    void applyPreferences();

public slots:
    /// Serial: open the port selected in the ConnectionBar with its settings; returns success
    /// (true when already open). SSH: start the connect sequence for the bar's target; returns
    /// false with a status message when there is no valid target, true once open() was started
    /// (the outcome arrives through connectionStateChanged()). A running log replay is stopped
    /// first (its usual "stopped" status line is emitted) so replayed and live bytes never
    /// interleave; see replayLogFile().
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
    /// SSH only: open the RemoteFileDialog (modeless, one per session, created on first use) in
    /// upload direction, pre-filled with `localPath` when given. Refused with a status message on
    /// a serial session or while not connected.
    void uploadFile(const QString& localPath = QString());
    /// SSH only: the RemoteFileDialog in download direction (same rules as uploadFile()).
    void downloadFile();
    /// statusMessage("BREAK sent") only when the connection asserted BREAK; failures surface via
    /// errorOccurred -> statusMessage. SSH: statusMessage("Not available for SSH sessions").
    void sendBreak();
    /// Serial: sends "stty cols <cols> rows <rows>\r" so a Linux shell over UART matches the widget.
    /// SSH: transport()->notifyTerminalSize() (a window-change request) and a status message.
    void syncTerminalSize();
    void sendBytes(const QByteArray& bytes);
    void sendQuickCommand(const QuickCommand& command);
    void focusTerminal();
    /// Replay a captured log file (raw or SessionLogger text capture) through the terminal,
    /// hex view and logger as if the device sent it (core/LogReplayer.h). An empty `path`
    /// opens a file dialog; `bytesPerSecond` < 0 asks the user for the speed (LogReplayer::
    /// standardSpeeds()), 0 = unlimited. Only while disconnected: when the transport is not
    /// Disconnected the request is refused with a statusMessage() (replayed bytes would
    /// interleave with live device output).
    void replayLogFile(const QString& path = QString(), qint64 bytesPerSecond = -1);
    void stopReplay();

signals:
    void titleChanged(const QString& title);
    void connectionStateChanged(Transport::State state);
    void statusMessage(const QString& message, int timeoutMs);
    void countersChanged(quint64 rx, quint64 tx);
    void loggingChanged(bool active, const QString& filePath);
    void viewModeChanged(SessionWidget::ViewMode mode);
    void gridSizeChanged(int rows, int cols);
    void quickCommandsEditRequested();
    void findRequested();   ///< terminal context menu "Find..."
    /// A log replay started (true) or finished/stopped (false); MainWindow refreshes its actions.
    void replayStateChanged(bool active);
    /// SSH bar gear: MainWindow opens the SshProfilesDialog.
    void sshProfilesEditRequested();

private slots:
    void onConnectionStateChanged(Transport::State state);
    void onConnectionError(const QString& message);
    void onConnectionLost(const QString& name);
    void onConnectionRestored(const QString& name);
    void onBarSettingsChanged(const SerialSettings& settings);
    void onPortsChanged(const QList<SerialPortEntry>& ports);
    void onInputSendRequested(const QByteArray& payload, const QString& displayText);
    // ---- SSH ----
    void onSshProfileChanged(const SshProfile& profile);
    void onHostKeyVerificationRequired(const SshConnection::HostKeyInfo& info);
    void onAuthPromptRequired(const SshConnection::AuthPrompt& prompt);
    void onBannerReceived(const QString& text);
    void onAuthenticated(const QString& method);
    void onShellStarted();
    void onChannelClosed(int exitStatus);

private:
    void init(SshProfileStore* profiles);        ///< shared constructor body
    void setupUi();
    /// Dim informational line in the terminal ("--- text ---"; `decorated` = false writes the text
    /// bare, for banner lines).
    void writeSystemLine(const QString& text, bool decorated = true);
    /// Plain-text payloads only (UTF-8 in): transcoded to the session encoding when it is not
    /// UTF-8. Never use it on bytes that may contain \xHH escapes; see unescapeForDevice().
    QByteArray encodeForDevice(const QByteArray& utf8) const;
    /// HexUtils::unescape(text, encodeText, error) with encodeText = the session encoding
    /// (UTF-8 when the encoding is empty, "UTF-8" or has no encoder): text runs are transcoded,
    /// \xHH bytes are appended verbatim. No line ending is added.
    QByteArray unescapeForDevice(const QString& text, QString* error) const;
    bool askReplaySpeed(qint64& bytesPerSecond);                 ///< QInputDialog over LogReplayer::standardSpeeds()
    bool connectSerial();                                        ///< connectPort() body, Kind::Serial
    bool connectSsh();                                           ///< connectPort() body, Kind::Ssh
    void startAutoLog(const QString& name);                      ///< the auto-log rules of connectPort()
    QString logBaseName() const;                                 ///< port name / sanitized SSH display name
    void applySshProfile(const SshProfile& profile);             ///< to the connection while disconnected
    bool sshTransferAllowed();                                   ///< uploadFile()/downloadFile() guard + message
    RemoteFileDialog* remoteFileDialog();                        ///< created on first use
    static QString sanitizeForFileName(const QString& name);     ///< [A-Za-z0-9._@-], the rest -> "_"

    Transport::Kind m_kind = Transport::Kind::Serial;
    Transport* m_transport = nullptr;        ///< m_connection or m_ssh
    SerialConnection* m_connection = nullptr;   ///< serial only
    SshConnection* m_ssh = nullptr;             ///< SSH only
    ConnectionBar* m_bar = nullptr;             ///< serial only
    SshConnectionBar* m_sshBar = nullptr;       ///< SSH only
    SshProfileStore* m_profiles = nullptr;      ///< SSH only, not owned, may be null
    QStackedWidget* m_stack = nullptr;
    TerminalWidget* m_terminal = nullptr;
    HexDumpView* m_hexView = nullptr;
    QuickCommandBar* m_quickBar = nullptr;
    CommandInput* m_input = nullptr;
    SessionLogger* m_logger = nullptr;
    QuickCommandStore* m_quickCommands = nullptr;
    CommandHistory* m_history = nullptr;
    SendFileDialog* m_sendFileDialog = nullptr;
    RemoteFileDialog* m_remoteFileDialog = nullptr;   ///< SSH only; created by uploadFile()/downloadFile()
    LogReplayer* m_replayer = nullptr;       ///< created on the first replayLogFile(); QObject child
    ViewMode m_viewMode = ViewMode::Terminal;
    /// Target (port name / SSH display name) the active log was auto-started for; empty when the
    /// log was started by the user or no log is active.
    QString m_autoLogPort;
    /// The state before the current one (Connecting -> Connected is a user-initiated SSH connect,
    /// Reconnecting -> Connected an automatic one; only the former starts the auto-log).
    Transport::State m_lastState = Transport::State::Disconnected;
    bool m_preferencesApplied = false;   ///< first applyPreferences() also seeds the command input line ending
    QFont m_appliedFont;                 ///< terminal font last pushed by applyPreferences() (keeps user zoom otherwise)
};
