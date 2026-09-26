#pragma once

#include <QMainWindow>
#include <QTranslator>
#include <QActionGroup>
#include <QTimer>

#include "core/CommandHistory.h"
#include "core/SerialConnection.h"
#include "core/Transport.h"
#include "ui/SessionWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QTabWidget;
class QLabel;
class QDockWidget;
class QuickCommandStore;
class SshProfileStore;
class SystemLogViewer;

/**
 * Top-level window: a QTabWidget of SessionWidgets (serial or SSH), menus/toolbar, status bar
 * and the System Log dock.
 *
 * MainWindow.ui defines the menu bar, tool bar, status bar and these actions (objectName):
 *   File:    actionNewSession (Ctrl+T), actionNewSshSession (Ctrl+Shift+T, icon :/icons/ssh.svg),
 *            actionCloseSession (Ctrl+W), actionStartLogging, actionStopLogging,
 *            actionOpenLogFolder, actionReplayLog (Ctrl+Shift+R), actionStopReplay,
 *            actionQuit (Ctrl+Shift+Q)
 *   Session: actionConnect (F2), actionDisconnect (F3), actionClear (Ctrl+Shift+L),
 *            actionResetTerminal, actionSendFile (Ctrl+Shift+O), actionUploadFile,
 *            actionDownloadFile (SFTP; enabled only for a connected SSH tab), actionSendBreak
 *            (serial only), actionSyncTerminalSize, actionRefreshPorts (F5, serial only)
 *   Edit:    actionCopy (Ctrl+Shift+C), actionPaste (Ctrl+Shift+V, enabled only while
 *            connected), actionSelectAll, actionFind (Ctrl+Shift+F), actionQuickCommands,
 *            actionSshProfiles, actionPreferences (Ctrl+,)
 *   View:    actionHexView (checkable, Ctrl+Shift+H), actionShowCommandInput (checkable),
 *            actionShowQuickCommands (checkable), actionPauseWhileSelecting (checkable, mirrors
 *            AppSettings::pauseWhileSelecting() both ways: toggling writes the setting, a settings
 *            change - e.g. from Preferences - re-checks the action), actionRightClickPastes
 *            (checkable, mirrors AppSettings::rightClickPastes() the same way: cmd.exe-style
 *            right click pastes / copies, Shift+right click opens the menu), actionSystemLog (checkable),
 *            actionZoomIn (Ctrl++), actionZoomOut (Ctrl+-), actionZoomReset (Ctrl+0),
 *            actionNextTab (Ctrl+Tab), actionPreviousTab (Ctrl+Shift+Tab)
 *   Language: actionLanguageEnglish, actionLanguageChinese (checkable, exclusive)
 *   Help:    actionVersion, actionHomepage, actionAbout
 * plus a central QTabWidget named tabWidget (movable, closable, document mode) with a "+"
 * corner button that creates a session.
 *
 * Shortcut rule: while a session is connected the terminal sends every bare Ctrl+<letter>
 * to the device (Ctrl+C = 0x03, Ctrl+L = 0x0C, ...), so no menu action may use one. Actions
 * that would clash use Ctrl+Shift+<letter> instead (Ctrl+Shift+T for New SSH Session is fine for
 * the same reason); Ctrl+T / Ctrl+W / Ctrl+Tab / Ctrl+, and the F-keys above are the only
 * exceptions. TerminalWidget passes exactly these keys (and every Ctrl+Shift+<letter>) through
 * to the application's shortcut map, even while connected.
 *
 * Behaviour:
 *  - Startup: restore geometry/state; if AppSettings::restoreLastPorts() reopen a tab per
 *    saved key (not connected) - a port name gives a serial tab, an SSH restore key
 *    (SessionWidget::isSshRestoreKey(): "ssh:profile:<id>" / "ssh:target:<user@host:port>") an
 *    SSH tab with that target - else one empty session pre-selecting lastPortName().
 *    newSession(key) does the same dispatch, so main.cpp and the persistence share one path.
 *  - SSH: newSshSession(target) adds an SSH tab (Kind::Ssh, the window's SshProfileStore
 *    m_sshProfiles - loaded in the constructor, saved 250 ms after every changed() and in
 *    closeEvent) with the target / restore key applied, or, without one, the last connected
 *    ad-hoc target from AppSettings::lastSshTarget() pre-filled (never auto-connecting), and
 *    focuses the target field. onSshProfiles() runs SshProfilesDialog modally (pre-selecting the
 *    current SSH tab's profile); "Connect" in the dialog opens a new SSH tab on that profile and
 *    connects it. The sessions' sshProfilesEditRequested (the bar's gear) opens the same dialog.
 *    Upload / Download forward to the current session's uploadFile() / downloadFile().
 *  - Tab text = session title; tab icon = coloured dot (green connected, amber
 *    connecting / reconnecting, grey disconnected) drawn with QPainter into a 10x10 QPixmap;
 *    tab tooltip = "<displayName> · <summary>\n<state>" (+ the log file while logging).
 *  - Status bar permanent widgets (right side): connection state + Transport::displayName() and
 *    summary() ("COM8 · 115200 8N1", "root@10.0.0.24 · ssh-ed25519 · publickey"), RX/TX counters
 *    ("RX 12.3 KB  TX 456 B"), grid size ("120x40"), encoding, logging indicator ("● LOG" when
 *    active, tooltip = path).
 *    Left side: transient statusMessage() from the active session; an empty message clears it
 *    (the session uses that when a paused terminal resumes). The active session's
 *    persistentStatusMessage() (the "Output paused" hint) is re-shown whenever a transient message
 *    expires and swapped for the new tab's own hint on every tab change, so a background or
 *    closed tab's hint never stays on screen.
 *  - Session/Edit/View actions act on currentSession(); enabled state is refreshed by
 *    updateActions() on tab change and connection state change.
 *  - Closing a connected tab, or the window with connected tabs, asks for confirmation when
 *    AppSettings::confirmCloseWhenConnected().
 *  - closeEvent saves geometry/state, the open sessions' restore keys (SessionWidget::portName()),
 *    history (dataDirectory()/history.txt), quick commands and the SSH profiles.
 *  - Language menu: QTranslator for app strings (:/translations/<code>.qm) + Qt base strings
 *    (embedded :/translations/qtbase_<code>.qm, falling back to qtbase_/qt_<code> in
 *    QLibraryInfo::path(TranslationsPath)); LanguageChange event -> ui->retranslateUi(this) +
 *    retranslateStatusBar(); child widgets handle their own changeEvent.
 *  - The window title is "BuildAI Serial Utility <version>" plus " - <displayName>" of the
 *    active tab (the port, or the SSH profile name / user@host:port).
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    SessionWidget* currentSession() const;
    SessionWidget* sessionAt(int index) const;
    int sessionCount() const;

public slots:
    /// A new tab: a serial session pre-selecting `portName`, or - when the name is an SSH restore
    /// key (SessionWidget::isSshRestoreKey()) - newSshSession(portName).
    SessionWidget* newSession(const QString& portName = QString());
    /// A new SSH tab; `target` is a restore key or plain "user@host[:port]" (empty: the last
    /// connected ad-hoc target is pre-filled). Never connects. Focuses the target field.
    SessionWidget* newSshSession(const QString& target = QString());
    void closeSession(int index);
    void closeCurrentSession();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onSessionTitleChanged(const QString& title);
    void onSessionStateChanged(Transport::State state);
    void onSessionStatusMessage(const QString& message, int timeoutMs);
    void onSessionCounters(quint64 rx, quint64 tx);
    void onSessionLoggingChanged(bool active, const QString& filePath);
    void onSessionGridSize(int rows, int cols);
    void onConnect();
    void onDisconnect();
    void onClear();
    void onResetTerminal();
    void onSendFile();
    void onSendBreak();
    void onSyncTerminalSize();
    void onRefreshPorts();
    void onStartLogging();
    void onStopLogging();
    void onOpenLogFolder();
    void onReplayLog();      ///< File > Replay Log File... (actionReplayLog, added at integration)
    void onStopReplay();     ///< File > Stop Replay (actionStopReplay)
    void onCopy();
    void onPaste();
    void onSelectAll();
    void onFind();
    void onHexViewToggled(bool on);
    void onShowCommandInputToggled(bool on);
    void onShowQuickCommandsToggled(bool on);
    void onPauseWhileSelectingToggled(bool on);   ///< View > Pause Output While Selecting -> AppSettings
    void syncPauseWhileSelectingAction();          ///< AppSettings::changed -> action checked state
    void onRightClickPastesToggled(bool on);       ///< View > Right Click Pastes -> AppSettings
    void syncRightClickPastesAction();             ///< AppSettings::changed -> action checked state
    void updateSessionHint();                      ///< tab change: current session's persistentStatusMessage()
    void onZoomIn();
    void onZoomOut();
    void onZoomReset();
    void onNextTab();
    void onPreviousTab();
    void onQuickCommands();
    void onSshProfiles();    ///< Edit > SSH Profiles... / the SSH bar's gear (modal SshProfilesDialog)
    void onUploadFile();     ///< Session > Upload File to Remote... -> currentSession()->uploadFile()
    void onDownloadFile();   ///< Session > Download File from Remote... -> currentSession()->downloadFile()
    void saveSshProfiles();  ///< debounced SshProfileStore::save()
    void onPreferences();
    void onPreferencesApplied();
    void onLanguageTriggered(QAction* action);
    void onAbout();
    void onVersion();
    void onHomepage();
    void updateActions();
    void updateStatusBar();
    void updateTabAppearance(SessionWidget* session);
    void updateWindowTitle();

private:
    void setupStatusBar();
    void setupSystemLogDock();
    void setupLanguageMenu();
    void switchLanguage(const QString& code);
    void retranslateStatusBar();
    void connectSession(SessionWidget* session);
    /// Shared tail of newSession()/newSshSession(): signals, panel visibility, tab, focus, refresh.
    void addSessionTab(SessionWidget* session, const QString& logName);
    int indexOf(SessionWidget* session) const;
    /// quitting=true is the closeEvent variant (title "Quit <app>", button "Quit"); false is the
    /// single-tab close variant (title "Close Session", button "Close").
    bool confirmCloseSessions(const QList<SessionWidget*>& connected, bool quitting = false);
    void saveState();
    void restoreState();
    static QIcon stateIcon(Transport::State state);
    static QString formatBytes(quint64 bytes);   ///< "456 B", "12.3 KB", "1.2 MB"

    Ui::MainWindow* ui = nullptr;
    QTabWidget* m_tabs = nullptr;
    QuickCommandStore* m_quickCommands = nullptr;
    SshProfileStore* m_sshProfiles = nullptr;    ///< QObject child; shared by every SSH tab and the profile dialog
    QTimer m_sshProfilesSaveTimer;               ///< single-shot 250 ms after SshProfileStore::changed()
    CommandHistory m_history;
    QDockWidget* m_systemLogDock = nullptr;
    SystemLogViewer* m_systemLog = nullptr;
    QTranslator m_appTranslator;
    QTranslator m_qtTranslator;
    QActionGroup* m_languageGroup = nullptr;
    QString m_currentLanguage;

    QLabel* m_statusConnection = nullptr;
    QLabel* m_statusCounters = nullptr;
    QLabel* m_statusGrid = nullptr;
    QLabel* m_statusEncoding = nullptr;
    QLabel* m_statusLogging = nullptr;

    // ---- Implementation state (private; added by the ui-main package) ------------------
    QString m_lastFindText;   ///< seed for the next Find dialog
    QString m_sessionHint;    ///< the current tab's persistentStatusMessage(); re-shown after transient messages
};
