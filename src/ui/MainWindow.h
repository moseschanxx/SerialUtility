#pragma once

#include <QMainWindow>
#include <QTranslator>
#include <QActionGroup>

#include "core/CommandHistory.h"
#include "core/SerialConnection.h"
#include "ui/SessionWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QTabWidget;
class QLabel;
class QDockWidget;
class QuickCommandStore;
class SystemLogViewer;

/**
 * Top-level window: a QTabWidget of SessionWidgets, menus/toolbar, status bar and the
 * System Log dock.
 *
 * MainWindow.ui defines the menu bar, tool bar, status bar and these actions (objectName):
 *   File:    actionNewSession (Ctrl+T), actionCloseSession (Ctrl+W), actionStartLogging,
 *            actionStopLogging, actionOpenLogFolder, actionReplayLog (Ctrl+Shift+R),
 *            actionStopReplay, actionQuit (Ctrl+Shift+Q)
 *   Session: actionConnect (F2), actionDisconnect (F3), actionClear (Ctrl+Shift+L),
 *            actionResetTerminal, actionSendFile (Ctrl+Shift+O), actionSendBreak,
 *            actionSyncTerminalSize, actionRefreshPorts (F5)
 *   Edit:    actionCopy (Ctrl+Shift+C), actionPaste (Ctrl+Shift+V), actionSelectAll,
 *            actionFind (Ctrl+Shift+F), actionQuickCommands, actionPreferences (Ctrl+,)
 *   View:    actionHexView (checkable, Ctrl+Shift+H), actionShowCommandInput (checkable),
 *            actionShowQuickCommands (checkable), actionSystemLog (checkable),
 *            actionZoomIn (Ctrl++), actionZoomOut (Ctrl+-), actionZoomReset (Ctrl+0),
 *            actionNextTab (Ctrl+Tab), actionPreviousTab (Ctrl+Shift+Tab)
 *   Language: actionLanguageEnglish, actionLanguageChinese (checkable, exclusive)
 *   Help:    actionAbout, actionVersion, actionHomepage
 * plus a central QTabWidget named tabWidget (movable, closable, document mode) with a "+"
 * corner button that creates a session.
 *
 * Shortcut rule: while a session is connected the terminal sends every bare Ctrl+<letter>
 * to the device (Ctrl+C = 0x03, Ctrl+L = 0x0C, ...), so no menu action may use one. Actions
 * that would clash use Ctrl+Shift+<letter> instead; Ctrl+T / Ctrl+W / Ctrl+Tab / Ctrl+, and
 * the F-keys above are the only exceptions. TerminalWidget passes exactly these keys (and every
 * Ctrl+Shift+<letter>) through to the application's shortcut map, even while connected.
 *
 * Behaviour:
 *  - Startup: restore geometry/state; if AppSettings::restoreLastPorts() reopen a tab per
 *    saved port (not connected), else one empty session pre-selecting lastPortName().
 *  - Tab text = session title; tab icon = coloured dot (green connected, amber
 *    reconnecting, grey disconnected) drawn with QPainter into a 10x10 QPixmap.
 *  - Status bar permanent widgets (right side): connection state + settings summary
 *    ("COM8 · 115200 8N1"), RX/TX counters ("RX 12.3 KB  TX 456 B"), grid size ("120x40"),
 *    encoding, logging indicator ("● LOG" when active, tooltip = path).
 *    Left side: transient statusMessage() from the active session.
 *  - Session/Edit/View actions act on currentSession(); enabled state is refreshed by
 *    updateActions() on tab change and connection state change.
 *  - Closing a connected tab, or the window with connected tabs, asks for confirmation when
 *    AppSettings::confirmCloseWhenConnected().
 *  - closeEvent saves geometry/state, open port names, history (dataDirectory()/history.txt)
 *    and quick commands.
 *  - Language menu: same approach as the Vispek reference (QTranslator for app + Qt base
 *    translations from QLibraryInfo::path(TranslationsPath)); LanguageChange event ->
 *    ui->retranslateUi(this) + retranslateStatusBar(); child widgets handle their own
 *    changeEvent.
 *  - The window title is "BuildAI Serial Utility <version>" plus " - <port>" of the active tab.
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
    SessionWidget* newSession(const QString& portName = QString());
    void closeSession(int index);
    void closeCurrentSession();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void onSessionTitleChanged(const QString& title);
    void onSessionStateChanged(SerialConnection::State state);
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
    void onZoomIn();
    void onZoomOut();
    void onZoomReset();
    void onNextTab();
    void onPreviousTab();
    void onQuickCommands();
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
    int indexOf(SessionWidget* session) const;
    bool confirmCloseSessions(const QList<SessionWidget*>& connected);
    void saveState();
    void restoreState();
    static QIcon stateIcon(SerialConnection::State state);
    static QString formatBytes(quint64 bytes);   ///< "456 B", "12.3 KB", "1.2 MB"

    Ui::MainWindow* ui = nullptr;
    QTabWidget* m_tabs = nullptr;
    QuickCommandStore* m_quickCommands = nullptr;
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
};
