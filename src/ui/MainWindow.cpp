#include "ui/MainWindow.h"
#include "ui_MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QInputDialog>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>

#include "Version.h"
#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/QuickCommand.h"
#include "core/SerialPortEnumerator.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/PreferencesDialog.h"
#include "dialogs/QuickCommandsDialog.h"
#include "dialogs/VersionDialog.h"
#include "terminal/TerminalWidget.h"
#include "ui/CommandInput.h"
#include "ui/HexDumpView.h"
#include "ui/QuickCommandBar.h"
#include "ui/SessionWidget.h"
#include "ui/SystemLogViewer.h"

namespace {

const QLatin1String kShowCommandInputKey("ui/showCommandInput");
const QLatin1String kShowQuickCommandsKey("ui/showQuickCommands");
const QLatin1String kLanguageEnglish("en_US");
const QLatin1String kLanguageChinese("zh_CN");

QString historyFilePath()
{
    return AppSettings::dataDirectory() + QStringLiteral("/history.txt");
}

/// Icon-theme lookup with a QStyle standard icon as fallback (Windows has no icon theme).
void setActionIcon(QAction* action, const char* themeName, QStyle::StandardPixmap fallback, const QStyle* style)
{
    const QIcon fallbackIcon = style ? style->standardIcon(fallback) : QIcon();
    action->setIcon(QIcon::fromTheme(QLatin1String(themeName), fallbackIcon));
}

/// "zh*" -> zh_CN, everything else -> en_US.
QString normalizeLanguage(const QString& code)
{
    if (code.startsWith(QLatin1String("zh"), Qt::CaseInsensitive)) {
        return kLanguageChinese;
    }
    return kLanguageEnglish;
}

QFrame* makeStatusSeparator(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    m_tabs = ui->tabWidget;

    // "+" corner button that opens a new session.
    auto* addButton = new QToolButton(m_tabs);
    addButton->setObjectName(QStringLiteral("newSessionButton"));
    addButton->setText(QStringLiteral("+"));
    addButton->setAutoRaise(true);
    addButton->setToolTip(tr("New Session (Ctrl+T)"));
    addButton->setFocusPolicy(Qt::NoFocus);
    m_tabs->setCornerWidget(addButton, Qt::TopRightCorner);
    connect(addButton, &QToolButton::clicked, this, [this]() { newSession(); });

    // Shared data.
    m_quickCommands = new QuickCommandStore(this);
    if (!m_quickCommands->load()) {
        qCWarning(lcUi) << "quick commands file is malformed, using current list";
    }
    m_history.load(historyFilePath());
    SerialPortEnumerator::instance().start();

    // Toolbar icons (Qt has no bundled icon theme on Windows -> QStyle fallbacks).
    const QStyle* s = style();
    setActionIcon(ui->actionNewSession, "tab-new", QStyle::SP_FileIcon, s);
    setActionIcon(ui->actionCloseSession, "tab-close", QStyle::SP_DialogCloseButton, s);
    setActionIcon(ui->actionConnect, "network-connect", QStyle::SP_DialogApplyButton, s);
    setActionIcon(ui->actionDisconnect, "network-disconnect", QStyle::SP_DialogCancelButton, s);
    setActionIcon(ui->actionClear, "edit-clear", QStyle::SP_TrashIcon, s);
    setActionIcon(ui->actionResetTerminal, "view-refresh", QStyle::SP_DialogResetButton, s);
    setActionIcon(ui->actionSendFile, "document-send", QStyle::SP_ArrowUp, s);
    setActionIcon(ui->actionHexView, "view-list-details", QStyle::SP_FileDialogDetailedView, s);
    setActionIcon(ui->actionStartLogging, "media-record", QStyle::SP_DialogSaveButton, s);
    setActionIcon(ui->actionStopLogging, "media-playback-stop", QStyle::SP_MediaStop, s);
    setActionIcon(ui->actionOpenLogFolder, "folder-open", QStyle::SP_DirOpenIcon, s);
    setActionIcon(ui->actionQuickCommands, "system-run", QStyle::SP_CommandLink, s);
    setActionIcon(ui->actionPreferences, "preferences-system", QStyle::SP_FileDialogInfoView, s);
    setActionIcon(ui->actionRefreshPorts, "view-refresh", QStyle::SP_BrowserReload, s);
    setActionIcon(ui->actionQuit, "application-exit", QStyle::SP_DialogCloseButton, s);
    setActionIcon(ui->actionAbout, "help-about", QStyle::SP_MessageBoxInformation, s);
    setActionIcon(ui->actionSystemLog, "utilities-terminal", QStyle::SP_MessageBoxWarning, s);

    setupStatusBar();
    setupSystemLogDock();
    setupLanguageMenu();
    // A transient message ("Not connected", "Reconnected to COM8", ...) replaces the current
    // session's lasting hint ("Output paused while selecting"); bring the hint back when the
    // transient one expires or is cleared, for as long as the session still reports it.
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString& message) {
        if (message.isEmpty() && !m_sessionHint.isEmpty()) {
            statusBar()->showMessage(m_sessionHint, 0);
        }
    });

    // ---- Tabs ----
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // ---- File ----
    connect(ui->actionNewSession, &QAction::triggered, this, [this]() { newSession(); });
    connect(ui->actionCloseSession, &QAction::triggered, this, &MainWindow::closeCurrentSession);
    connect(ui->actionStartLogging, &QAction::triggered, this, &MainWindow::onStartLogging);
    connect(ui->actionStopLogging, &QAction::triggered, this, &MainWindow::onStopLogging);
    connect(ui->actionOpenLogFolder, &QAction::triggered, this, &MainWindow::onOpenLogFolder);
    connect(ui->actionReplayLog, &QAction::triggered, this, &MainWindow::onReplayLog);
    connect(ui->actionStopReplay, &QAction::triggered, this, &MainWindow::onStopReplay);
    connect(ui->actionQuit, &QAction::triggered, this, &QWidget::close);

    // ---- Session ----
    connect(ui->actionConnect, &QAction::triggered, this, &MainWindow::onConnect);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::onDisconnect);
    connect(ui->actionClear, &QAction::triggered, this, &MainWindow::onClear);
    connect(ui->actionResetTerminal, &QAction::triggered, this, &MainWindow::onResetTerminal);
    connect(ui->actionSendFile, &QAction::triggered, this, &MainWindow::onSendFile);
    connect(ui->actionSendBreak, &QAction::triggered, this, &MainWindow::onSendBreak);
    connect(ui->actionSyncTerminalSize, &QAction::triggered, this, &MainWindow::onSyncTerminalSize);
    connect(ui->actionRefreshPorts, &QAction::triggered, this, &MainWindow::onRefreshPorts);

    // ---- Edit ----
    connect(ui->actionCopy, &QAction::triggered, this, &MainWindow::onCopy);
    connect(ui->actionPaste, &QAction::triggered, this, &MainWindow::onPaste);
    connect(ui->actionSelectAll, &QAction::triggered, this, &MainWindow::onSelectAll);
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::onFind);
    connect(ui->actionQuickCommands, &QAction::triggered, this, &MainWindow::onQuickCommands);
    connect(ui->actionPreferences, &QAction::triggered, this, &MainWindow::onPreferences);

    // ---- View ----
    connect(ui->actionHexView, &QAction::toggled, this, &MainWindow::onHexViewToggled);
    connect(ui->actionShowCommandInput, &QAction::toggled, this, &MainWindow::onShowCommandInputToggled);
    connect(ui->actionShowQuickCommands, &QAction::toggled, this, &MainWindow::onShowQuickCommandsToggled);
    connect(ui->actionPauseWhileSelecting, &QAction::toggled, this, &MainWindow::onPauseWhileSelectingToggled);
    connect(ui->actionRightClickPastes, &QAction::toggled, this, &MainWindow::onRightClickPastesToggled);
    // The Preferences dialog writes the same settings: keep the menu actions in step with it.
    connect(&AppSettings::instance(), &AppSettings::changed, this, [this](const QString&) {
        syncPauseWhileSelectingAction();
        syncRightClickPastesAction();
    });
    connect(ui->actionZoomIn, &QAction::triggered, this, &MainWindow::onZoomIn);
    connect(ui->actionZoomOut, &QAction::triggered, this, &MainWindow::onZoomOut);
    connect(ui->actionZoomReset, &QAction::triggered, this, &MainWindow::onZoomReset);
    connect(ui->actionNextTab, &QAction::triggered, this, &MainWindow::onNextTab);
    connect(ui->actionPreviousTab, &QAction::triggered, this, &MainWindow::onPreviousTab);

    // ---- Help ----
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onAbout);
    connect(ui->actionVersion, &QAction::triggered, this, &MainWindow::onVersion);
    connect(ui->actionHomepage, &QAction::triggered, this, &MainWindow::onHomepage);

    // Every port hot-plug refreshes the Connect enable state (port may have appeared).
    connect(&SerialPortEnumerator::instance(), &SerialPortEnumerator::portsChanged, this,
            [this](const QList<SerialPortEntry>&) { updateActions(); });

    restoreState();

    // ---- Sessions to open at startup ----
    const AppSettings& settings = AppSettings::instance();
    QStringList ports;
    if (settings.restoreLastPorts()) {
        ports = settings.lastOpenPorts();
        ports.removeAll(QString());
        ports.removeDuplicates();
    }
    if (ports.isEmpty()) {
        newSession(settings.lastPortName());
    } else {
        int preferred = 0;
        for (int i = 0; i < ports.size(); ++i) {
            newSession(ports.at(i));
            if (ports.at(i) == settings.lastPortName()) {
                preferred = i;
            }
        }
        m_tabs->setCurrentIndex(preferred);
    }

    updateActions();
    updateStatusBar();
    updateWindowTitle();

    qCInfo(lcUi) << "main window ready," << sessionCount() << "session(s)";
}

MainWindow::~MainWindow()
{
    delete ui;
    ui = nullptr;
}

// ---------------------------------------------------------------------------------------
// Session access
// ---------------------------------------------------------------------------------------

SessionWidget* MainWindow::currentSession() const
{
    return m_tabs ? qobject_cast<SessionWidget*>(m_tabs->currentWidget()) : nullptr;
}

SessionWidget* MainWindow::sessionAt(int index) const
{
    if (!m_tabs || index < 0 || index >= m_tabs->count()) {
        return nullptr;
    }
    return qobject_cast<SessionWidget*>(m_tabs->widget(index));
}

int MainWindow::sessionCount() const
{
    return m_tabs ? m_tabs->count() : 0;
}

int MainWindow::indexOf(SessionWidget* session) const
{
    return (m_tabs && session) ? m_tabs->indexOf(session) : -1;
}

// ---------------------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------------------

SessionWidget* MainWindow::newSession(const QString& portName)
{
    auto* session = new SessionWidget(m_quickCommands, &m_history, this);
    if (!portName.isEmpty()) {
        session->setPortName(portName);
    }
    connectSession(session);

    // Panel visibility follows the (persisted) View menu state.
    if (session->commandInput()) {
        session->commandInput()->setVisible(ui->actionShowCommandInput->isChecked());
    }
    if (session->quickCommandBar()) {
        session->quickCommandBar()->setVisible(ui->actionShowQuickCommands->isChecked());
    }

    const int index = m_tabs->addTab(session, stateIcon(SerialConnection::State::Disconnected), session->title());
    updateTabAppearance(session);
    m_tabs->setCurrentIndex(index);
    session->focusTerminal();

    qCInfo(lcUi) << "new session" << (portName.isEmpty() ? QStringLiteral("(no port)") : portName);

    updateActions();
    updateStatusBar();
    updateWindowTitle();
    return session;
}

void MainWindow::closeSession(int index)
{
    SessionWidget* session = sessionAt(index);
    if (!session) {
        return;
    }

    if (session->isConnected() && AppSettings::instance().confirmCloseWhenConnected()) {
        if (!confirmCloseSessions({session})) {
            return;
        }
    }

    qCInfo(lcUi) << "closing session" << session->title();
    if (session->isLogging()) {
        session->stopLogging();
    }
    session->disconnectPort();

    m_tabs->removeTab(index);
    session->setParent(nullptr);
    session->deleteLater();

    if (m_tabs->count() == 0) {
        newSession();   // the window always has at least one tab
    }

    updateActions();
    updateStatusBar();
    updateWindowTitle();
}

void MainWindow::closeCurrentSession()
{
    if (m_tabs->currentIndex() >= 0) {
        closeSession(m_tabs->currentIndex());
    }
}

void MainWindow::connectSession(SessionWidget* session)
{
    connect(session, &SessionWidget::titleChanged, this, &MainWindow::onSessionTitleChanged);
    connect(session, &SessionWidget::connectionStateChanged, this, &MainWindow::onSessionStateChanged);
    connect(session, &SessionWidget::statusMessage, this, &MainWindow::onSessionStatusMessage);
    connect(session, &SessionWidget::countersChanged, this, &MainWindow::onSessionCounters);
    connect(session, &SessionWidget::loggingChanged, this, &MainWindow::onSessionLoggingChanged);
    connect(session, &SessionWidget::gridSizeChanged, this, &MainWindow::onSessionGridSize);
    connect(session, &SessionWidget::quickCommandsEditRequested, this, &MainWindow::onQuickCommands);
    connect(session, &SessionWidget::findRequested, this, &MainWindow::onFind);
    connect(session, &SessionWidget::viewModeChanged, this, [this, session](SessionWidget::ViewMode) {
        if (session == currentSession()) {
            updateActions();
        }
    });
    connect(session, &SessionWidget::replayStateChanged, this, [this, session](bool) {
        if (session == currentSession()) {
            updateActions();
        }
    });
}

bool MainWindow::confirmCloseSessions(const QList<SessionWidget*>& connected, bool quitting)
{
    if (connected.isEmpty()) {
        return true;
    }

    QStringList names;
    for (SessionWidget* session : connected) {
        names.append(session->title());
    }
    const QString joined = names.join(QStringLiteral(", "));
    const int count = static_cast<int>(connected.size());

    QString title;
    QString text;
    QString yesLabel;
    if (quitting) {
        title = tr("Quit %1").arg(QStringLiteral(APP_DISPLAY_NAME));
        text = tr("%n session(s) are still connected (%1).\nQuit anyway? All tabs will be closed and logging stopped.",
                  nullptr, count)
                   .arg(joined);
        yesLabel = tr("Quit");
    } else {
        title = tr("Close Session");
        text = count == 1 ? tr("Session %1 is still connected.\nClose it anyway?").arg(names.first())
                          : tr("%n sessions are still connected (%1).\nClose them anyway?", nullptr, count).arg(joined);
        yesLabel = tr("Close");
    }

    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.button(QMessageBox::Yes)->setText(yesLabel);
    return box.exec() == QMessageBox::Yes;
}

// ---------------------------------------------------------------------------------------
// Window events
// ---------------------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent* event)
{
    QList<SessionWidget*> connected;
    for (int i = 0; i < sessionCount(); ++i) {
        if (SessionWidget* session = sessionAt(i); session && session->isConnected()) {
            connected.append(session);
        }
    }

    if (!connected.isEmpty() && AppSettings::instance().confirmCloseWhenConnected() &&
        !confirmCloseSessions(connected, /*quitting=*/true)) {
        event->ignore();
        return;
    }

    saveState();

    for (int i = 0; i < sessionCount(); ++i) {
        if (SessionWidget* session = sessionAt(i)) {
            if (session->isLogging()) {
                session->stopLogging();
            }
            session->disconnectPort();
        }
    }

    qCInfo(lcApp) << "main window closed";
    event->accept();
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
        if (m_systemLogDock) {
            m_systemLogDock->setWindowTitle(tr("System Log"));
        }
        if (auto* addButton = m_tabs ? m_tabs->cornerWidget(Qt::TopRightCorner) : nullptr) {
            addButton->setToolTip(tr("New Session (Ctrl+T)"));
        }
        retranslateStatusBar();
        updateWindowTitle();
        for (int i = 0; i < sessionCount(); ++i) {
            updateTabAppearance(sessionAt(i));
        }
        updateSessionHint();   // the "Output paused" hint, if shown, in the new language
    }
    QMainWindow::changeEvent(event);
}

// ---------------------------------------------------------------------------------------
// Session signal handlers
// ---------------------------------------------------------------------------------------

void MainWindow::onTabChanged(int index)
{
    updateActions();
    updateStatusBar();
    updateWindowTitle();
    updateSessionHint();
    if (SessionWidget* session = sessionAt(index)) {
        session->focusTerminal();
    }
}

void MainWindow::updateSessionHint()
{
    // The "Output paused" hint belongs to the session that posted it: it must not outlive a tab
    // switch or a close, and the new current tab shows its own hint (if it is paused as well).
    const QString previous = m_sessionHint;
    SessionWidget* session = currentSession();
    m_sessionHint = session ? session->persistentStatusMessage() : QString();
    if (m_sessionHint == previous) {
        return;
    }
    if (!m_sessionHint.isEmpty()) {
        statusBar()->showMessage(m_sessionHint, 0);
    } else if (statusBar()->currentMessage() == previous) {
        statusBar()->clearMessage();
    }
}

void MainWindow::onTabCloseRequested(int index)
{
    closeSession(index);
}

void MainWindow::onSessionTitleChanged(const QString& title)
{
    auto* session = qobject_cast<SessionWidget*>(sender());
    if (!session) {
        return;
    }
    const int index = indexOf(session);
    if (index >= 0) {
        m_tabs->setTabText(index, title);
    }
    updateTabAppearance(session);
    if (session == currentSession()) {
        updateActions();
        updateStatusBar();
        updateWindowTitle();
    }
}

void MainWindow::onSessionStateChanged(SerialConnection::State state)
{
    auto* session = qobject_cast<SessionWidget*>(sender());
    if (!session) {
        return;
    }
    qCDebug(lcUi) << "session" << session->title() << "->" << state;
    updateTabAppearance(session);
    if (session == currentSession()) {
        updateActions();
        updateStatusBar();
        updateWindowTitle();
    }
}

void MainWindow::onSessionStatusMessage(const QString& message, int timeoutMs)
{
    auto* session = qobject_cast<SessionWidget*>(sender());
    if (!session || session != currentSession()) {
        return;
    }
    // Whatever arrived, remember the session's lasting hint (empty once a paused terminal
    // resumed) so the messageChanged handler restores exactly the current state.
    m_sessionHint = session->persistentStatusMessage();
    if (message.isEmpty()) {
        statusBar()->clearMessage();   // e.g. the "Output paused" hint once the terminal resumes
    } else {
        statusBar()->showMessage(message, timeoutMs);
    }
}

void MainWindow::onSessionCounters(quint64 rx, quint64 tx)
{
    auto* session = qobject_cast<SessionWidget*>(sender());
    if (session && session == currentSession() && m_statusCounters) {
        m_statusCounters->setText(tr("RX %1  TX %2").arg(formatBytes(rx), formatBytes(tx)));
    }
}

void MainWindow::onSessionLoggingChanged(bool active, const QString& filePath)
{
    auto* session = qobject_cast<SessionWidget*>(sender());
    if (!session) {
        return;
    }
    qCInfo(lcUi) << "logging" << (active ? "started" : "stopped") << filePath;
    updateTabAppearance(session);   // the tab tooltip shows "Logging to <file>" while active
    if (session == currentSession()) {
        updateActions();
        updateStatusBar();
    }
}

void MainWindow::onSessionGridSize(int rows, int cols)
{
    auto* session = qobject_cast<SessionWidget*>(sender());
    if (session && session == currentSession() && m_statusGrid) {
        m_statusGrid->setText(QStringLiteral("%1x%2").arg(cols).arg(rows));
    }
}

// ---------------------------------------------------------------------------------------
// Session / File actions
// ---------------------------------------------------------------------------------------

void MainWindow::onConnect()
{
    if (SessionWidget* session = currentSession()) {
        session->connectPort();
    }
}

void MainWindow::onDisconnect()
{
    if (SessionWidget* session = currentSession()) {
        session->disconnectPort();
    }
}

void MainWindow::onClear()
{
    if (SessionWidget* session = currentSession()) {
        session->clearTerminal();
    }
}

void MainWindow::onResetTerminal()
{
    if (SessionWidget* session = currentSession()) {
        session->resetTerminal();
    }
}

void MainWindow::onSendFile()
{
    if (SessionWidget* session = currentSession()) {
        session->sendFile();
    }
}

void MainWindow::onSendBreak()
{
    if (SessionWidget* session = currentSession()) {
        session->sendBreak();
    }
}

void MainWindow::onSyncTerminalSize()
{
    if (SessionWidget* session = currentSession()) {
        session->syncTerminalSize();
    }
}

void MainWindow::onRefreshPorts()
{
    SerialPortEnumerator& enumerator = SerialPortEnumerator::instance();
    enumerator.refresh();
    const int count = static_cast<int>(enumerator.ports().size());
    statusBar()->showMessage(tr("%n serial port(s) found", nullptr, count), 3000);
    updateActions();
}

void MainWindow::onStartLogging()
{
    if (SessionWidget* session = currentSession()) {
        session->startLogging();
    }
}

void MainWindow::onStopLogging()
{
    if (SessionWidget* session = currentSession()) {
        session->stopLogging();
    }
}

void MainWindow::onOpenLogFolder()
{
    const QString dir = AppSettings::instance().logDirectory();
    if (!QDir().mkpath(dir)) {
        statusBar()->showMessage(tr("Cannot create log folder %1").arg(QDir::toNativeSeparators(dir)), 5000);
        qCWarning(lcUi) << "cannot create log directory" << dir;
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        statusBar()->showMessage(tr("Cannot open %1").arg(QDir::toNativeSeparators(dir)), 5000);
        qCWarning(lcUi) << "QDesktopServices::openUrl failed for" << dir;
    }
}

void MainWindow::onReplayLog()
{
    if (SessionWidget* session = currentSession()) {
        session->replayLogFile();   // file dialog + speed prompt
    }
}

void MainWindow::onStopReplay()
{
    if (SessionWidget* session = currentSession()) {
        session->stopReplay();
    }
}

// ---------------------------------------------------------------------------------------
// Edit actions
// ---------------------------------------------------------------------------------------

void MainWindow::onCopy()
{
    SessionWidget* session = currentSession();
    if (!session) {
        return;
    }
    if (session->viewMode() == SessionWidget::ViewMode::HexDump && session->hexView()) {
        session->hexView()->copy();
    } else if (session->terminal()) {
        session->terminal()->copySelection();
    }
}

void MainWindow::onPaste()
{
    SessionWidget* session = currentSession();
    if (!session || !session->terminal()) {
        return;
    }
    if (!session->isConnected()) {   // unreachable from the menu (updateActions gates Paste), keyboard-safe
        statusBar()->showMessage(tr("Not connected"), 3000);
        return;
    }
    session->terminal()->paste();
}

void MainWindow::onSelectAll()
{
    SessionWidget* session = currentSession();
    if (!session) {
        return;
    }
    if (session->viewMode() == SessionWidget::ViewMode::HexDump && session->hexView()) {
        session->hexView()->selectAll();
    } else if (session->terminal()) {
        session->terminal()->selectAll();
    }
}

void MainWindow::onFind()
{
    SessionWidget* session = currentSession();
    if (!session || !session->terminal()) {
        return;
    }

    bool ok = false;
    const QString text =
        QInputDialog::getText(this, tr("Find"), tr("Find in terminal output:"), QLineEdit::Normal, m_lastFindText, &ok);
    if (!ok || text.isEmpty()) {
        return;
    }
    m_lastFindText = text;

    if (session->viewMode() != SessionWidget::ViewMode::Terminal) {
        session->setViewMode(SessionWidget::ViewMode::Terminal);
    }
    if (!session->terminal()->findNext(text)) {
        statusBar()->showMessage(tr("Not found"), 3000);
    }
}

void MainWindow::onHexViewToggled(bool on)
{
    if (SessionWidget* session = currentSession()) {
        const auto mode = on ? SessionWidget::ViewMode::HexDump : SessionWidget::ViewMode::Terminal;
        if (session->viewMode() != mode) {
            session->setViewMode(mode);
        }
    }
    updateActions();
}

void MainWindow::onShowCommandInputToggled(bool on)
{
    for (int i = 0; i < sessionCount(); ++i) {
        if (SessionWidget* session = sessionAt(i); session && session->commandInput()) {
            session->commandInput()->setVisible(on);
        }
    }
    QSettings().setValue(kShowCommandInputKey, on);
}

void MainWindow::onShowQuickCommandsToggled(bool on)
{
    for (int i = 0; i < sessionCount(); ++i) {
        if (SessionWidget* session = sessionAt(i); session && session->quickCommandBar()) {
            session->quickCommandBar()->setVisible(on);
        }
    }
    QSettings().setValue(kShowQuickCommandsKey, on);
}

void MainWindow::onPauseWhileSelectingToggled(bool on)
{
    // AppSettings::changed -> every SessionWidget::applyPreferences() pushes it to its terminal.
    AppSettings::instance().setPauseWhileSelecting(on);
    qCInfo(lcUi) << "pause output while selecting" << (on ? "on" : "off");
}

void MainWindow::syncPauseWhileSelectingAction()
{
    const QSignalBlocker blocker(ui->actionPauseWhileSelecting);
    ui->actionPauseWhileSelecting->setChecked(AppSettings::instance().pauseWhileSelecting());
}

void MainWindow::onRightClickPastesToggled(bool on)
{
    // AppSettings::changed -> every SessionWidget::applyPreferences() pushes it to its terminal.
    AppSettings::instance().setRightClickPastes(on);
    qCInfo(lcUi) << "right click pastes (cmd.exe style)" << (on ? "on" : "off");
}

void MainWindow::syncRightClickPastesAction()
{
    const QSignalBlocker blocker(ui->actionRightClickPastes);
    ui->actionRightClickPastes->setChecked(AppSettings::instance().rightClickPastes());
}

void MainWindow::onZoomIn()
{
    if (SessionWidget* session = currentSession(); session && session->terminal()) {
        session->terminal()->zoomIn();
    }
}

void MainWindow::onZoomOut()
{
    if (SessionWidget* session = currentSession(); session && session->terminal()) {
        session->terminal()->zoomOut();
    }
}

void MainWindow::onZoomReset()
{
    if (SessionWidget* session = currentSession(); session && session->terminal()) {
        session->terminal()->resetZoom();
    }
}

void MainWindow::onNextTab()
{
    const int count = m_tabs->count();
    if (count > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % count);
    }
}

void MainWindow::onPreviousTab()
{
    const int count = m_tabs->count();
    if (count > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + count - 1) % count);
    }
}

void MainWindow::onQuickCommands()
{
    QuickCommandsDialog dialog(m_quickCommands, this);
    if (dialog.exec() == QDialog::Accepted) {
        qCInfo(lcUi) << "quick commands updated:" << m_quickCommands->commands().size() << "entries";
    }
}

void MainWindow::onPreferences()
{
    PreferencesDialog dialog(this);
    connect(&dialog, &PreferencesDialog::applied, this, &MainWindow::onPreferencesApplied);
    dialog.exec();
}

void MainWindow::onPreferencesApplied()
{
    for (int i = 0; i < sessionCount(); ++i) {
        if (SessionWidget* session = sessionAt(i)) {
            session->applyPreferences();
        }
    }
    updateActions();
    updateStatusBar();
    qCInfo(lcApp) << "preferences applied";
}

// ---------------------------------------------------------------------------------------
// Language
// ---------------------------------------------------------------------------------------

void MainWindow::setupLanguageMenu()
{
    m_languageGroup = new QActionGroup(this);
    m_languageGroup->setExclusive(true);
    ui->actionLanguageEnglish->setData(QString(kLanguageEnglish));
    ui->actionLanguageChinese->setData(QString(kLanguageChinese));
    m_languageGroup->addAction(ui->actionLanguageEnglish);
    m_languageGroup->addAction(ui->actionLanguageChinese);
    connect(m_languageGroup, &QActionGroup::triggered, this, &MainWindow::onLanguageTriggered);

    QString code = AppSettings::instance().language();
    if (code.isEmpty()) {
        code = QLocale::system().name();
    }
    code = normalizeLanguage(code);

    if (code == kLanguageChinese) {
        ui->actionLanguageChinese->setChecked(true);
    } else {
        ui->actionLanguageEnglish->setChecked(true);
    }
    switchLanguage(code);
}

void MainWindow::onLanguageTriggered(QAction* action)
{
    if (!action) {
        return;
    }
    const QString code = action->data().toString();
    if (!code.isEmpty() && code != m_currentLanguage) {
        switchLanguage(code);
    }
}

void MainWindow::switchLanguage(const QString& code)
{
    qApp->removeTranslator(&m_appTranslator);
    qApp->removeTranslator(&m_qtTranslator);

    // Application strings. Failure for en_US is normal: it is the source language.
    if (m_appTranslator.load(QStringLiteral(":/translations/%1.qm").arg(code))) {
        qApp->installTranslator(&m_appTranslator);
    } else if (code != kLanguageEnglish) {
        qCWarning(lcApp) << "no application translation for" << code;
    }

    // Qt's own dialogs / buttons. Preferred: the qtbase catalogue embedded at build time
    // (see CMakeLists.txt). Fallbacks: the Qt translations dir - an SDK has qtbase_<code>.qm,
    // a windeployqt tree has the merged qt_<code>.qm - trying full code then bare language.
    const QString language = code.section(QLatin1Char('_'), 0, 0);
    const QString qtDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    bool qtLoaded = m_qtTranslator.load(QStringLiteral(":/translations/qtbase_%1.qm").arg(code));
    for (const QString& name : {QStringLiteral("qtbase_") + code, QStringLiteral("qt_") + code,
                                QStringLiteral("qtbase_") + language, QStringLiteral("qt_") + language}) {
        if (qtLoaded) {
            break;
        }
        qtLoaded = m_qtTranslator.load(name, qtDir);
    }
    if (qtLoaded) {
        qApp->installTranslator(&m_qtTranslator);
    } else if (code != kLanguageEnglish) {
        qCWarning(lcApp) << "no Qt base translation for" << code << "(resource or" << qtDir << ")";
    }

    m_currentLanguage = code;
    AppSettings::instance().setLanguage(code);
    qCInfo(lcApp) << "language:" << code;
}

// ---------------------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------------------

void MainWindow::onAbout()
{
    if (auto* existing = findChild<AboutDialog*>(QString(), Qt::FindDirectChildrenOnly)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new AboutDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::onVersion()
{
    if (auto* existing = findChild<VersionDialog*>(QString(), Qt::FindDirectChildrenOnly)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new VersionDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::onHomepage()
{
    if (!QDesktopServices::openUrl(QUrl(QStringLiteral(APP_HOMEPAGE)))) {
        statusBar()->showMessage(tr("Cannot open %1").arg(QStringLiteral(APP_HOMEPAGE)), 5000);
    }
}

// ---------------------------------------------------------------------------------------
// Enable state / appearance
// ---------------------------------------------------------------------------------------

void MainWindow::updateActions()
{
    SessionWidget* session = currentSession();
    const bool hasSession = session != nullptr;
    const bool connected = hasSession && session->isConnected();
    const SerialConnection::State state =
        (hasSession && session->connection()) ? session->connection()->state() : SerialConnection::State::Disconnected;
    const bool reconnecting = state == SerialConnection::State::Reconnecting;
    const bool portSelected = hasSession && !session->portName().isEmpty();
    const bool logging = hasSession && session->isLogging();
    const bool hexMode = hasSession && session->viewMode() == SessionWidget::ViewMode::HexDump;

    ui->actionCloseSession->setEnabled(hasSession);
    ui->actionConnect->setEnabled(hasSession && !connected && !reconnecting && portSelected);
    ui->actionDisconnect->setEnabled(connected || reconnecting);
    ui->actionSendFile->setEnabled(connected);
    ui->actionSendBreak->setEnabled(connected);
    ui->actionSyncTerminalSize->setEnabled(connected);
    ui->actionClear->setEnabled(hasSession);
    ui->actionResetTerminal->setEnabled(hasSession);
    ui->actionStartLogging->setEnabled(hasSession && !logging);
    ui->actionStopLogging->setEnabled(hasSession && logging);
    ui->actionReplayLog->setEnabled(hasSession);
    ui->actionStopReplay->setEnabled(hasSession && session->isReplaying());

    ui->actionCopy->setEnabled(hasSession);
    ui->actionPaste->setEnabled(connected);   // the terminal drops input unless connected
    ui->actionSelectAll->setEnabled(hasSession);
    ui->actionFind->setEnabled(hasSession);

    {
        const QSignalBlocker blocker(ui->actionHexView);
        ui->actionHexView->setEnabled(hasSession);
        ui->actionHexView->setChecked(hexMode);
    }
    ui->actionZoomIn->setEnabled(hasSession);
    ui->actionZoomOut->setEnabled(hasSession);
    ui->actionZoomReset->setEnabled(hasSession);

    const bool severalTabs = sessionCount() > 1;
    ui->actionNextTab->setEnabled(severalTabs);
    ui->actionPreviousTab->setEnabled(severalTabs);
}

void MainWindow::updateTabAppearance(SessionWidget* session)
{
    const int index = indexOf(session);
    if (index < 0) {
        return;
    }
    const SerialConnection::State state =
        session->connection() ? session->connection()->state() : SerialConnection::State::Disconnected;

    m_tabs->setTabText(index, session->title());
    m_tabs->setTabIcon(index, stateIcon(state));

    QString tip = SerialConnection::stateText(state);
    if (session->connection() && !session->portName().isEmpty()) {
        tip = QStringLiteral("%1 · %2\n%3").arg(session->portName(), session->connection()->settings().summary(), tip);
    }
    if (session->isLogging()) {
        tip += QLatin1Char('\n') + tr("Logging to %1").arg(QDir::toNativeSeparators(session->logFilePath()));
    }
    m_tabs->setTabToolTip(index, tip);
}

void MainWindow::updateWindowTitle()
{
    QString title = QStringLiteral("%1 %2").arg(QStringLiteral(APP_DISPLAY_NAME), QStringLiteral(APP_VERSION));
    if (SessionWidget* session = currentSession(); session && !session->portName().isEmpty()) {
        title += QStringLiteral(" - ") + session->portName();
    }
    setWindowTitle(title);
}

QIcon MainWindow::stateIcon(SerialConnection::State state)
{
    QColor color;
    switch (state) {
    case SerialConnection::State::Connected:
        color = QColor(0x3C, 0xB0, 0x43);   // green
        break;
    case SerialConnection::State::Reconnecting:
        color = QColor(0xF0, 0xA0, 0x30);   // amber
        break;
    case SerialConnection::State::Disconnected:
        color = QColor(0x90, 0x90, 0x90);   // grey
        break;
    }

    QPixmap pixmap(10, 10);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(color.darker(130), 1.0));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(1.0, 1.0, 8.0, 8.0));
    }
    return QIcon(pixmap);
}

QString MainWindow::formatBytes(quint64 bytes)
{
    constexpr quint64 kKiB = 1024;
    constexpr quint64 kMiB = 1024 * 1024;
    if (bytes < kKiB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double value = static_cast<double>(bytes);
    if (bytes < kMiB) {
        return QStringLiteral("%1 KB").arg(QString::number(value / static_cast<double>(kKiB), 'f', 1));
    }
    return QStringLiteral("%1 MB").arg(QString::number(value / static_cast<double>(kMiB), 'f', 1));
}

// ---------------------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------------------

void MainWindow::setupStatusBar()
{
    QStatusBar* bar = statusBar();

    m_statusConnection = new QLabel(bar);
    m_statusConnection->setObjectName(QStringLiteral("statusConnectionLabel"));
    m_statusCounters = new QLabel(bar);
    m_statusCounters->setObjectName(QStringLiteral("statusCountersLabel"));
    m_statusGrid = new QLabel(bar);
    m_statusGrid->setObjectName(QStringLiteral("statusGridLabel"));
    m_statusEncoding = new QLabel(bar);
    m_statusEncoding->setObjectName(QStringLiteral("statusEncodingLabel"));
    m_statusLogging = new QLabel(bar);
    m_statusLogging->setObjectName(QStringLiteral("statusLoggingLabel"));

    for (QLabel* label : {m_statusConnection, m_statusCounters, m_statusGrid, m_statusEncoding, m_statusLogging}) {
        label->setContentsMargins(4, 0, 4, 0);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    m_statusLogging->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));

    bar->addPermanentWidget(m_statusConnection);
    bar->addPermanentWidget(makeStatusSeparator(bar));
    bar->addPermanentWidget(m_statusCounters);
    bar->addPermanentWidget(makeStatusSeparator(bar));
    bar->addPermanentWidget(m_statusGrid);
    bar->addPermanentWidget(makeStatusSeparator(bar));
    bar->addPermanentWidget(m_statusEncoding);
    bar->addPermanentWidget(makeStatusSeparator(bar));
    bar->addPermanentWidget(m_statusLogging);

    retranslateStatusBar();
}

void MainWindow::retranslateStatusBar()
{
    if (m_statusConnection) {
        m_statusConnection->setToolTip(tr("Connection state and line settings"));
    }
    if (m_statusCounters) {
        m_statusCounters->setToolTip(tr("Bytes received / transmitted in this session"));
    }
    if (m_statusGrid) {
        m_statusGrid->setToolTip(tr("Terminal size (columns x rows)"));
    }
    if (m_statusEncoding) {
        m_statusEncoding->setToolTip(tr("Text encoding"));
    }
    updateStatusBar();
}

void MainWindow::updateStatusBar()
{
    if (!m_statusConnection) {
        return;
    }

    SessionWidget* session = currentSession();
    if (!session) {
        m_statusConnection->setText(tr("No session"));
        m_statusCounters->setText(tr("RX %1  TX %2").arg(formatBytes(0), formatBytes(0)));
        m_statusGrid->clear();
        m_statusEncoding->clear();
        m_statusLogging->clear();
        m_statusLogging->setToolTip(QString());
        return;
    }

    SerialConnection* connection = session->connection();
    const SerialConnection::State state = connection ? connection->state() : SerialConnection::State::Disconnected;
    QString connectionText = SerialConnection::stateText(state);
    if (!session->portName().isEmpty()) {
        const QString summary = connection ? connection->settings().summary() : QString();
        connectionText = QStringLiteral("%1   %2 · %3").arg(connectionText, session->portName(), summary);
    }
    m_statusConnection->setText(connectionText);

    const quint64 rx = connection ? connection->bytesReceived() : 0;
    const quint64 tx = connection ? connection->bytesSent() : 0;
    m_statusCounters->setText(tr("RX %1  TX %2").arg(formatBytes(rx), formatBytes(tx)));

    if (TerminalWidget* terminal = session->terminal()) {
        m_statusGrid->setText(QStringLiteral("%1x%2").arg(terminal->columns()).arg(terminal->visibleRows()));
        m_statusEncoding->setText(terminal->encoding());
    } else {
        m_statusGrid->clear();
        m_statusEncoding->clear();
    }

    if (session->isLogging()) {
        m_statusLogging->setText(QStringLiteral("● ") + tr("LOG"));
        m_statusLogging->setToolTip(QDir::toNativeSeparators(session->logFilePath()));
    } else {
        m_statusLogging->clear();
        m_statusLogging->setToolTip(QString());
    }
}

// ---------------------------------------------------------------------------------------
// System Log dock
// ---------------------------------------------------------------------------------------

void MainWindow::setupSystemLogDock()
{
    m_systemLogDock = new QDockWidget(tr("System Log"), this);
    m_systemLogDock->setObjectName(QStringLiteral("systemLogDock"));
    m_systemLogDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea |
                                     Qt::RightDockWidgetArea);
    m_systemLogDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                 QDockWidget::DockWidgetFloatable);

    m_systemLog = new SystemLogViewer(m_systemLogDock);
    m_systemLog->setObjectName(QStringLiteral("systemLogViewer"));
    SystemLogViewer::setInstance(m_systemLog);
    m_systemLogDock->setWidget(m_systemLog);

    addDockWidget(Qt::BottomDockWidgetArea, m_systemLogDock);
    m_systemLogDock->hide();

    // Keep the menu action and the dock's own toggle action in sync (two-way).
    QAction* toggle = m_systemLogDock->toggleViewAction();
    ui->actionSystemLog->setChecked(toggle->isChecked());
    connect(toggle, &QAction::toggled, ui->actionSystemLog, &QAction::setChecked);
    connect(ui->actionSystemLog, &QAction::toggled, this, [this](bool on) {
        QAction* dockToggle = m_systemLogDock->toggleViewAction();
        if (dockToggle->isChecked() != on) {
            dockToggle->trigger();
        }
    });
}

// ---------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------

void MainWindow::saveState()
{
    AppSettings& settings = AppSettings::instance();
    settings.setMainWindowGeometry(saveGeometry());
    settings.setMainWindowState(QMainWindow::saveState());

    QStringList ports;
    for (int i = 0; i < sessionCount(); ++i) {
        if (SessionWidget* session = sessionAt(i); session && !session->portName().isEmpty()) {
            ports.append(session->portName());
        }
    }
    settings.setLastOpenPorts(ports);
    if (SessionWidget* current = currentSession(); current && !current->portName().isEmpty()) {
        settings.setLastPortName(current->portName());
    }

    if (!m_history.save(historyFilePath())) {
        qCWarning(lcApp) << "cannot save command history to" << historyFilePath();
    }
    if (!m_quickCommands->save()) {
        qCWarning(lcApp) << "cannot save quick commands to" << QuickCommandStore::defaultFilePath();
    }
    settings.sync();
}

void MainWindow::restoreState()
{
    const AppSettings& settings = AppSettings::instance();
    const QByteArray geometry = settings.mainWindowGeometry();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const QByteArray state = settings.mainWindowState();
    if (!state.isEmpty()) {
        QMainWindow::restoreState(state);
    }

    QSettings qsettings;
    ui->actionShowCommandInput->setChecked(qsettings.value(kShowCommandInputKey, true).toBool());
    ui->actionShowQuickCommands->setChecked(qsettings.value(kShowQuickCommandsKey, true).toBool());
    syncPauseWhileSelectingAction();   // AppSettings::pauseWhileSelecting(), default on
    syncRightClickPastesAction();      // AppSettings::rightClickPastes(), default on
}
