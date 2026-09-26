#include "ui/SessionWidget.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QLocale>
#include <QPointer>
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
#include "dialogs/AuthPromptDialog.h"
#include "dialogs/HostKeyDialog.h"
#include "dialogs/RemoteFileDialog.h"
#include "dialogs/SendFileDialog.h"
#include "ssh/SshProfile.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalTheme.h"
#include "terminal/TerminalWidget.h"
#include "ui/CommandInput.h"
#include "ui/ConnectionBar.h"
#include "ui/HexDumpView.h"
#include "ui/QuickCommandBar.h"
#include "ui/SshConnectionBar.h"

namespace {

constexpr int kStatusShortMs = 3000;
constexpr int kStatusLongMs = 5000;

const QLatin1String kProfileKeyPrefix("ssh:profile:");
const QLatin1String kTargetKeyPrefix("ssh:target:");

/// The logging category of a transport kind. Callable so it fits the qC* macros, which invoke
/// their category argument: qCInfo(transportLog(m_kind)) would not compile, a lambda does.
const QLoggingCategory& transportCategory(Transport::Kind kind)
{
    return kind == Transport::Kind::Ssh ? lcSsh() : lcSerial();
}

} // namespace

// The qC* macros call `category()`; this member is that callable for the session's transport.
#define SU_LC ([this]() -> const QLoggingCategory& { return transportCategory(m_kind); })

SessionWidget::SessionWidget(QuickCommandStore* quickCommands, CommandHistory* history, QWidget* parent)
    : SessionWidget(Transport::Kind::Serial, quickCommands, history, nullptr, parent)
{
}

SessionWidget::SessionWidget(Transport::Kind kind, QuickCommandStore* quickCommands, CommandHistory* history,
                             SshProfileStore* profiles, QWidget* parent)
    : QWidget(parent)
    , m_kind(kind)
    , m_quickCommands(quickCommands)
    , m_history(history)
{
    init(profiles);
}

void SessionWidget::init(SshProfileStore* profiles)
{
    if (m_kind == Transport::Kind::Ssh) {
        m_ssh = new SshConnection(this);
        m_transport = m_ssh;
        m_profiles = profiles;
    } else {
        m_connection = new SerialConnection(this);
        m_transport = m_connection;
    }
    m_logger = new SessionLogger(this);

    setupUi();

    if (m_connection) {
        // Start from the user's default line parameters (no port selected yet).
        SerialSettings defaults = AppSettings::instance().defaultSerialSettings();
        defaults.portName.clear();
        m_bar->setSettings(defaults);
        m_connection->setSettings(defaults);
    }
    if (m_sshBar && m_profiles) {
        m_sshBar->setStore(m_profiles);
        // The bar rebuilds silently on store changes (rename, delete, import in the profiles
        // dialog); the connection's profile copy - the title and the restore key - follows it
        // while disconnected. Connected after the bar's own slot, so the bar has rebuilt (and
        // fallen back to the target of a deleted profile) by the time this runs.
        connect(m_profiles, &SshProfileStore::changed, this, [this] {
            if (m_transport->state() == Transport::State::Disconnected && m_sshBar->hasValidTarget()) {
                applySshProfile(m_sshBar->currentProfile());
            }
        });
    }

    // ---- Transport -> views / logger ----------------------------------------------------
    connect(m_transport, &Transport::dataReceived, m_terminal, &TerminalWidget::feedData);
    connect(m_transport, &Transport::dataReceived, m_hexView, &HexDumpView::appendReceived);
    connect(m_transport, &Transport::dataReceived, m_logger, &SessionLogger::logReceived);
    connect(m_transport, &Transport::dataSent, m_hexView, &HexDumpView::appendSent);
    connect(m_transport, &Transport::dataSent, m_logger, &SessionLogger::logSent);
    connect(m_transport, &Transport::stateChanged, this, &SessionWidget::onConnectionStateChanged);
    connect(m_transport, &Transport::errorOccurred, this, &SessionWidget::onConnectionError);
    connect(m_transport, &Transport::connectionLost, this, &SessionWidget::onConnectionLost);
    connect(m_transport, &Transport::connectionRestored, this, &SessionWidget::onConnectionRestored);
    connect(m_transport, &Transport::countersChanged, this, &SessionWidget::countersChanged);
    if (m_connection) {
        connect(m_connection, &SerialConnection::pinsChanged, m_bar, &ConnectionBar::setPinStates);
    }

    // ---- Terminal -----------------------------------------------------------------------
    connect(m_terminal, &TerminalWidget::sendData, this, &SessionWidget::sendBytes);
    connect(m_terminal, &TerminalWidget::fileDropped, this, [this](const QString& path) {
        if (isSsh()) {
            uploadFile(path);
        } else {
            sendFile(path);
        }
    });
    connect(m_terminal, &TerminalWidget::syncSizeRequested, this, &SessionWidget::syncTerminalSize);
    connect(m_terminal, &TerminalWidget::findRequested, this, &SessionWidget::findRequested);
    connect(m_terminal, &TerminalWidget::gridSizeChanged, this, &SessionWidget::gridSizeChanged);
    // The transport learns every grid change: SSH turns it into a window-change request, serial ignores it.
    connect(m_terminal, &TerminalWidget::gridSizeChanged, this,
            [this](int rows, int cols) { m_transport->notifyTerminalSize(cols, rows); });
    connect(m_terminal, &TerminalWidget::titleChanged, this, [this](const QString& deviceTitle) {
        if (!deviceTitle.trimmed().isEmpty()) {
            emit statusMessage(deviceTitle, kStatusShortMs);
        }
    });
    // Mark mode (pause output while selecting): a persistent status-bar hint while the display is
    // frozen; the empty message with timeout 0 on resume clears it again (MainWindow keeps it in
    // step with the current tab through persistentStatusMessage()).
    connect(m_terminal, &TerminalWidget::outputPausedChanged, this,
            [this](bool) { emit statusMessage(persistentStatusMessage(), 0); });
    connect(m_terminal, &TerminalWidget::pauseBufferOverflow, this, [this](qint64 flushedBytes) {
        qCInfo(lcUi) << "terminal pause buffer overflow:" << flushedBytes << "bytes flushed";
        emit statusMessage(tr("Output resumed: %1 arrived while the display was paused")
                               .arg(QLocale().formattedDataSize(flushedBytes, 1, QLocale::DataSizeTraditionalFormat)),
                           kStatusLongMs);
    });

    // ---- Command input / quick commands ---------------------------------------------------
    connect(m_input, &CommandInput::sendRequested, this, &SessionWidget::onInputSendRequested);
    connect(m_quickBar, &QuickCommandBar::commandTriggered, this, &SessionWidget::sendQuickCommand);
    connect(m_quickBar, &QuickCommandBar::editRequested, this, &SessionWidget::quickCommandsEditRequested);

    // ---- Serial connection bar ------------------------------------------------------------
    if (m_bar) {
        connect(m_bar, &ConnectionBar::connectRequested, this, [this]() { connectPort(); });
        connect(m_bar, &ConnectionBar::disconnectRequested, this, &SessionWidget::disconnectPort);
        connect(m_bar, &ConnectionBar::settingsChanged, this, &SessionWidget::onBarSettingsChanged);
        connect(m_bar, &ConnectionBar::dtrToggled, m_connection, &SerialConnection::setDtr);
        connect(m_bar, &ConnectionBar::rtsToggled, m_connection, &SerialConnection::setRts);
        connect(m_bar, &ConnectionBar::sendBreakRequested, this, &SessionWidget::sendBreak);
        connect(m_bar, &ConnectionBar::refreshRequested, &SerialPortEnumerator::instance(),
                &SerialPortEnumerator::refresh);
        connect(&SerialPortEnumerator::instance(), &SerialPortEnumerator::portsChanged, this,
                &SessionWidget::onPortsChanged);
    }

    // ---- SSH connection bar / connection --------------------------------------------------
    if (m_sshBar) {
        connect(m_sshBar, &SshConnectionBar::connectRequested, this, [this]() { connectPort(); });
        connect(m_sshBar, &SshConnectionBar::disconnectRequested, this, &SessionWidget::disconnectPort);
        connect(m_sshBar, &SshConnectionBar::profilesEditRequested, this, &SessionWidget::sshProfilesEditRequested);
        connect(m_sshBar, &SshConnectionBar::profileChanged, this, &SessionWidget::onSshProfileChanged);
    }
    if (m_ssh) {
        connect(m_ssh, &SshConnection::hostKeyVerificationRequired, this,
                &SessionWidget::onHostKeyVerificationRequired);
        connect(m_ssh, &SshConnection::authPromptRequired, this, &SessionWidget::onAuthPromptRequired);
        connect(m_ssh, &SshConnection::bannerReceived, this, &SessionWidget::onBannerReceived);
        connect(m_ssh, &SshConnection::authenticated, this, &SessionWidget::onAuthenticated);
        connect(m_ssh, &SshConnection::shellStarted, this, &SessionWidget::onShellStarted);
        connect(m_ssh, &SshConnection::channelClosed, this, &SessionWidget::onChannelClosed);
    }

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

    onConnectionStateChanged(m_transport->state());
    applyPreferences();
}

SessionWidget::~SessionWidget()
{
    // The widget is going away: stop forwarding state/log signals to a half-destroyed owner.
    disconnect(m_transport, nullptr, this, nullptr);
    disconnect(m_logger, nullptr, this, nullptr);
    // Children are destroyed in creation order (connection and logger first). Closing the
    // window makes the connection bar's baud line edit lose focus, which emits
    // editingFinished -> settingsChanged, and the input strips can emit on focus loss as
    // well; none of that may reach slots that touch the already destroyed connection.
    for (QObject* strip : {static_cast<QObject*>(m_bar), static_cast<QObject*>(m_sshBar),
                           static_cast<QObject*>(m_input), static_cast<QObject*>(m_quickBar),
                           static_cast<QObject*>(m_terminal)}) {
        if (strip) {
            disconnect(strip, nullptr, this, nullptr);
        }
    }
    if (m_replayer) {
        disconnect(m_replayer, nullptr, this, nullptr);
    }
    // The transfer dialog watches the SSH connection: take it down before the connection goes.
    delete m_remoteFileDialog;
    m_remoteFileDialog = nullptr;
    if (m_logger->isActive()) {
        m_logger->stop();
    }
    m_transport->close();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void SessionWidget::setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    QWidget* strip = nullptr;
    if (m_kind == Transport::Kind::Ssh) {
        m_sshBar = new SshConnectionBar(this);
        strip = m_sshBar;
    } else {
        m_bar = new ConnectionBar(this);
        strip = m_bar;
    }

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

    layout->addWidget(strip);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_quickBar);
    layout->addWidget(m_input);

    setFocusProxy(m_terminal);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Transport* SessionWidget::transport() const
{
    return m_transport;
}

SerialConnection* SessionWidget::connection() const
{
    return m_connection;
}

SshConnection* SessionWidget::sshConnection() const
{
    return m_ssh;
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

SshConnectionBar* SessionWidget::sshConnectionBar() const
{
    return m_sshBar;
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

Transport::Kind SessionWidget::kind() const
{
    return m_kind;
}

bool SessionWidget::isSsh() const
{
    return m_kind == Transport::Kind::Ssh;
}

QString SessionWidget::title() const
{
    if (isReplaying()) {
        return tr("Replay: %1").arg(QFileInfo(m_replayer->options().filePath).fileName());
    }
    if (isSsh()) {
        const QString name = m_ssh->profile().isValid() ? m_transport->displayName() : QString();
        return name.isEmpty() ? tr("New SSH Session") : name;
    }
    const QString port = m_transport->displayName();
    return port.isEmpty() ? tr("New Session") : port;
}

bool SessionWidget::isConnected() const
{
    return m_transport->isOpen();
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
    if (isSsh()) {
        qCDebug(lcSsh) << "setPortName(" << portName << ") ignored on an SSH session; use setSshTarget()";
        return;
    }
    m_bar->selectPort(portName);
    SerialSettings settings = m_connection->settings();
    settings.portName = m_bar->selectedPortName();
    m_connection->setSettings(settings);
    m_bar->setConnectionState(m_connection->state()); // refresh the connect button dot
    emit titleChanged(title());
}

QString SessionWidget::portName() const
{
    return isSsh() ? restoreKey() : m_bar->selectedPortName();
}

QString SessionWidget::restoreKey() const
{
    if (!isSsh()) {
        return m_bar->selectedPortName();
    }
    const SshProfile profile = m_ssh->profile();
    if (!profile.id.isEmpty()) {
        return kProfileKeyPrefix + profile.id;
    }
    if (profile.isValid()) {
        return kTargetKeyPrefix + profile.displayTarget();
    }
    return {};
}

bool SessionWidget::isSshRestoreKey(const QString& key)
{
    return key.startsWith(kProfileKeyPrefix) || key.startsWith(kTargetKeyPrefix);
}

void SessionWidget::setSshTarget(const QString& keyOrTarget)
{
    if (!isSsh()) {
        qCDebug(lcSsh) << "setSshTarget(" << keyOrTarget << ") ignored on a serial session";
        return;
    }
    QString text = keyOrTarget.trimmed();
    if (text.startsWith(kProfileKeyPrefix)) {
        const QString id = text.mid(kProfileKeyPrefix.size());
        const std::optional<SshProfile> stored = m_profiles ? m_profiles->profile(id) : std::nullopt;
        if (!stored) {
            qCWarning(lcSsh) << "cannot restore SSH profile" << id << "- it no longer exists";
            return;
        }
        m_sshBar->selectProfile(id);
        applySshProfile(*stored);
        return;
    }
    if (text.startsWith(kTargetKeyPrefix)) {
        text = text.mid(kTargetKeyPrefix.size());
    }
    SshProfile profile;
    if (!SshProfile::parseTarget(text, profile)) {
        qCWarning(lcSsh) << "not an SSH target:" << text;
        return;
    }
    m_sshBar->setTarget(profile.displayTarget());
    applySshProfile(profile);
}

void SessionWidget::applySshProfile(const SshProfile& profile)
{
    if (m_transport->state() != Transport::State::Disconnected) {
        qCDebug(lcSsh) << "profile change ignored while" << Transport::stateText(m_transport->state());
        return;
    }
    m_ssh->setProfile(profile);
    emit titleChanged(title());
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
    m_terminal->setPauseWhileSelecting(s.pauseWhileSelecting());
    m_terminal->setRightClickPastes(s.rightClickPastes());

    m_transport->setAutoReconnect(s.autoReconnect());
    m_transport->setReconnectIntervalMs(s.reconnectIntervalMs());

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
    return isSsh() ? connectSsh() : connectSerial();
}

bool SessionWidget::connectSerial()
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

    startAutoLog(settings.portName);

    focusTerminal();
    return true;
}

bool SessionWidget::connectSsh()
{
    if (m_transport->state() != Transport::State::Disconnected) {
        return true;   // connected, or a connect / reconnect already in progress
    }
    if (!m_sshBar->hasValidTarget()) {
        emit statusMessage(tr("Enter a target such as user@host"), kStatusShortMs);
        m_sshBar->setFocusToTarget();
        return false;
    }
    SshProfile profile = m_sshBar->currentProfile();

    if (isReplaying()) {
        qCInfo(lcApp) << "stopping replay before connecting to" << profile.displayTarget();
        stopReplay();
    }

    // Preferences > SSH fills what the profile leaves open: the known_hosts file for every
    // profile, and for an ad-hoc target (no stored fields at all) the identity file, terminal
    // type and keep-alive as well.
    const AppSettings& s = AppSettings::instance();
    if (profile.knownHostsFile.trimmed().isEmpty()) {
        profile.knownHostsFile = s.sshKnownHostsFile();
    }
    if (profile.id.isEmpty()) {
        if (profile.identityFile.trimmed().isEmpty()) {
            profile.identityFile = s.sshDefaultIdentityFile();
        }
        profile.terminalType = s.sshDefaultTerminalType();
        profile.keepAliveSeconds = s.sshDefaultKeepAliveSeconds();
    }
    m_ssh->setProfile(profile);

    if (m_profiles) {
        if (!profile.id.isEmpty()) {
            m_profiles->touch(profile.id);
        } else {
            m_profiles->addRecentTarget(profile.displayTarget());
        }
    }
    AppSettings::instance().setLastSshTarget(restoreKey());

    qCInfo(lcSsh) << "connecting to" << profile.displayTarget();
    if (!m_ssh->open()) {
        // Normally the connection explains itself through errorOccurred(); cover a silent refusal.
        if (m_ssh->errorString().isEmpty()) {
            emit statusMessage(tr("Cannot connect to %1").arg(profile.displayTarget()), kStatusLongMs);
        }
        return false;
    }
    emit statusMessage(tr("Connecting to %1...").arg(profile.displayTarget()), kStatusShortMs);

    // The auto-log starts in onConnectionStateChanged(Connected): only then does summary() carry
    // the host-key type and auth method for the header, and a failed connect leaves no file.
    focusTerminal();
    return true;
}

void SessionWidget::startAutoLog(const QString& name)
{
    if (!AppSettings::instance().autoLog()) {
        return;
    }
    // A log the user started by hand is kept; an auto-started log for a *different* target is
    // closed so the new session gets its own "<name>_<time>.log" with a matching header.
    const bool autoLogForOther = m_logger->isActive() && !m_autoLogPort.isEmpty() && m_autoLogPort != name;
    if (!m_logger->isActive() || autoLogForOther) {
        // startLoggingTo() stops the old log first (stopped -> m_autoLogPort cleared), so the
        // assignment below re-establishes the association for the new file.
        startLoggingTo(SessionLogger::suggestFileName(name, AppSettings::instance().logDirectory()));
        if (m_logger->isActive()) {
            m_autoLogPort = name;
        }
    }
}

void SessionWidget::disconnectPort()
{
    const Transport::State before = m_transport->state();
    const qint64 dropped = m_transport->pendingTxBytes();
    m_transport->close();
    if (before != Transport::State::Disconnected) {
        const QString name = m_transport->displayName();
        qCInfo(SU_LC) << "closed" << name;
        if (dropped > 0) {
            writeSystemLine(tr("%1 unsent bytes discarded on disconnect").arg(dropped));
            emit statusMessage(tr("Disconnected from %1 (%2 unsent bytes discarded)").arg(name).arg(dropped),
                               kStatusLongMs);
        } else {
            emit statusMessage(tr("Disconnected from %1").arg(name), kStatusShortMs);
        }
    }
}

void SessionWidget::toggleConnection()
{
    // Anything but Disconnected (connected, connecting, reconnecting) is cancelled by the toggle.
    if (m_transport->state() != Transport::State::Disconnected) {
        disconnectPort();
    } else {
        connectPort();
    }
}

void SessionWidget::clearTerminal()
{
    // "Clear" means nothing of the previous output survives: screen, scrollback and hex dump.
    // The emulator state (attributes, modes) stays; Reset Terminal is the full RIS.
    m_terminal->clearAll();
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

QString SessionWidget::logBaseName() const
{
    return isSsh() ? sanitizeForFileName(m_transport->displayName()) : m_bar->selectedPortName();
}

void SessionWidget::startLogging()
{
    const QString base = logBaseName();
    const QString port = base.isEmpty() ? QStringLiteral("session") : base;
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
    const QString name = m_transport->displayName();
    const QString port = name.isEmpty() ? tr("(no port)") : name;
    const QString header = QStringLiteral("%1 %2").arg(port, m_transport->summary());

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
        // Backpressure: the dialog learns how much of what it queued is still in the transport's
        // write buffer, after every chunk and whenever the driver drains some of it.
        connect(m_sendFileDialog, &SendFileDialog::sendChunk, this, [this](const QByteArray& chunk) {
            sendBytes(chunk);
            m_sendFileDialog->updatePendingTx(m_transport->pendingTxBytes());
        });
        connect(m_transport, &Transport::txBytesWritten, m_sendFileDialog,
                [this](qint64) { m_sendFileDialog->updatePendingTx(m_transport->pendingTxBytes()); });
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

bool SessionWidget::sshTransferAllowed()
{
    if (!isSsh()) {
        emit statusMessage(tr("File transfer is only available for SSH sessions"), kStatusShortMs);
        return false;
    }
    if (!m_transport->isOpen()) {
        emit statusMessage(tr("Not connected"), kStatusShortMs);
        return false;
    }
    return true;
}

RemoteFileDialog* SessionWidget::remoteFileDialog()
{
    if (!m_remoteFileDialog) {
        m_remoteFileDialog = new RemoteFileDialog(m_ssh, this);
        m_remoteFileDialog->setModal(false);
        connect(m_remoteFileDialog, &RemoteFileDialog::transferFinished, this,
                [this](bool ok, const QString& message) {
                    qCInfo(lcSsh) << "file transfer finished:" << ok << message;
                    if (!message.isEmpty()) {
                        emit statusMessage(message, kStatusLongMs);
                    }
                });
    }
    return m_remoteFileDialog;
}

void SessionWidget::uploadFile(const QString& localPath)
{
    if (!sshTransferAllowed()) {
        return;
    }
    RemoteFileDialog* dialog = remoteFileDialog();
    dialog->setDirection(SshConnection::TransferDirection::Upload);
    if (!localPath.isEmpty()) {
        dialog->setLocalPath(localPath);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void SessionWidget::downloadFile()
{
    if (!sshTransferAllowed()) {
        return;
    }
    RemoteFileDialog* dialog = remoteFileDialog();
    dialog->setDirection(SshConnection::TransferDirection::Download);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void SessionWidget::sendBreak()
{
    if (isSsh()) {
        emit statusMessage(tr("Not available for SSH sessions"), kStatusShortMs);
        return;
    }
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
    if (isSsh()) {
        // A window-change request on the PTY (or the size remembered for the next open); nothing
        // is typed into the shell.
        m_transport->notifyTerminalSize(cols, rows);
        emit statusMessage(m_transport->isOpen() ? tr("Terminal size sent to the server") : tr("Not connected"),
                           kStatusShortMs);
        return;
    }
    sendBytes(QStringLiteral("stty cols %1 rows %2\r").arg(cols).arg(rows).toLatin1());
}

void SessionWidget::sendBytes(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    if (!m_transport->isOpen()) {
        emit statusMessage(tr("Not connected"), kStatusShortMs);
        return;
    }
    if (m_transport->write(bytes) < 0) {
        emit statusMessage(tr("Write to %1 failed").arg(m_transport->displayName()), kStatusLongMs);
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

void SessionWidget::onConnectionStateChanged(Transport::State state)
{
    const bool connected = (state == Transport::State::Connected);

    if (m_bar) {
        m_bar->setConnectionState(state);
        if (connected) {
            m_bar->setPinStates(m_connection->dtr(), m_connection->rts());
        }
    }
    if (m_sshBar) {
        m_sshBar->setConnectionState(state);
        m_sshBar->setSummary(connected ? m_transport->summary() : QString());
    }
    m_terminal->setInputEnabled(connected);
    m_input->setEnabledForConnection(connected);
    m_quickBar->setEnabledForConnection(connected);
    if (m_sendFileDialog) {
        m_sendFileDialog->setConnected(connected);
    }

    if (m_ssh && connected) {
        // Connected is reported after shellStarted() and is the first moment the facade accepts
        // the request: pre-fetch the remote home so RemoteFileDialog can propose <home>/<file>.
        m_ssh->requestRemoteHome();
        if (m_lastState == Transport::State::Connecting) {
            // A connect started by the user (not an automatic reconnect), now that summary() is
            // complete for the log header; see connectSsh().
            startAutoLog(sanitizeForFileName(m_transport->displayName()));
        }
    }
    m_lastState = state;

    emit connectionStateChanged(state);
    emit titleChanged(title());
}

void SessionWidget::onConnectionError(const QString& message)
{
    qCWarning(SU_LC) << message;
    emit statusMessage(message, kStatusLongMs);
}

void SessionWidget::onConnectionLost(const QString& name)
{
    // Info only: the transport already logged the warning with the underlying error.
    if (isSsh()) {
        qCInfo(lcSsh) << "connection to" << name << "lost";
        if (m_transport->autoReconnect()) {
            writeSystemLine(tr("connection to %1 lost, waiting to reconnect").arg(name));
            emit statusMessage(tr("Connection to %1 lost - reconnecting").arg(name), kStatusLongMs);
        } else {
            writeSystemLine(tr("connection to %1 lost").arg(name));
            emit statusMessage(tr("Connection to %1 lost").arg(name), kStatusLongMs);
        }
        return;
    }
    qCInfo(lcSerial) << "port" << name << "disappeared";
    if (m_transport->autoReconnect()) {
        writeSystemLine(tr("port %1 disappeared, waiting to reconnect").arg(name));
        emit statusMessage(tr("Port %1 disappeared - waiting for it to come back").arg(name), kStatusLongMs);
    } else {
        writeSystemLine(tr("port %1 disappeared").arg(name));
        emit statusMessage(tr("Port %1 disappeared").arg(name), kStatusLongMs);
    }
}

void SessionWidget::onConnectionRestored(const QString& name)
{
    qCInfo(SU_LC) << "reconnected to" << name;
    writeSystemLine(tr("reconnected"));
    emit statusMessage(tr("Reconnected to %1").arg(name), kStatusShortMs);
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
// SSH slots
// ---------------------------------------------------------------------------

void SessionWidget::onSshProfileChanged(const SshProfile& profile)
{
    applySshProfile(profile);
}

void SessionWidget::onHostKeyVerificationRequired(const SshConnection::HostKeyInfo& info)
{
    // The dialog blocks in a nested event loop; the session (and with it the connection that
    // waits for the answer) may be closed meanwhile, so both are tracked with QPointers and the
    // dialog lives on the heap: a parent that dies during exec() would take a stack dialog with it.
    QPointer<SessionWidget> self(this);
    QPointer<HostKeyDialog> dialog(new HostKeyDialog(info, window()));
    const int result = dialog->exec();
    if (!self || !dialog) {
        delete dialog;
        return;
    }
    const bool accepted = (result == QDialog::Accepted);
    const bool remember = accepted && dialog->remember();
    delete dialog;
    qCInfo(lcSsh) << "host key" << info.keyType << info.fingerprintSha256 << (accepted ? "accepted" : "rejected")
                  << (remember ? "(remembered)" : "");
    m_ssh->answerHostKey(accepted, remember);
}

void SessionWidget::onAuthPromptRequired(const SshConnection::AuthPrompt& prompt)
{
    QPointer<SessionWidget> self(this);
    QPointer<AuthPromptDialog> dialog(new AuthPromptDialog(prompt, window()));
    const int result = dialog->exec();
    if (!self || !dialog) {
        delete dialog;
        return;
    }
    if (result == QDialog::Accepted) {
        const QString response = dialog->response();
        const bool remember = dialog->remember();
        delete dialog;
        m_ssh->answerPrompt(response, remember);
    } else {
        delete dialog;
        qCInfo(lcSsh) << "authentication prompt cancelled";
        m_ssh->cancelPrompt();
    }
}

void SessionWidget::onBannerReceived(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();   // a banner ends in a newline; no blank trailer
    }
    for (QString line : lines) {
        line.remove(QLatin1Char('\r'));
        writeSystemLine(line, /*decorated=*/false);
    }
}

void SessionWidget::onAuthenticated(const QString& method)
{
    qCInfo(lcSsh) << "authenticated with" << method;
    emit statusMessage(tr("Authenticated (%1)").arg(method), kStatusShortMs);
}

void SessionWidget::onShellStarted()
{
    const SshProfile profile = m_ssh->profile();
    writeSystemLine(
        tr("connected to %1 (%2, %3)").arg(profile.displayTarget(), m_ssh->hostKeyType(), m_ssh->authMethod()));
    if (m_sshBar) {
        m_sshBar->setSummary(m_transport->summary());
    }
    m_transport->notifyTerminalSize(m_terminal->columns(), m_terminal->visibleRows());
    // The remote home is requested from onConnectionStateChanged(Connected): the facade still
    // reports Connecting here and would refuse the request.
}

void SessionWidget::onChannelClosed(int exitStatus)
{
    qCInfo(lcSsh) << "channel closed, exit status" << exitStatus;
    writeSystemLine(exitStatus >= 0 ? tr("connection closed (exit status %1)").arg(exitStatus)
                                    : tr("connection closed"));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SessionWidget::writeSystemLine(const QString& text, bool decorated)
{
    // Dim (SGR 2) on its own line, encoded like device output so the parser decodes it correctly.
    // It takes the feedData() path so it queues like device output while the display is paused
    // (writing straight into the parser would move the screen under a selection in progress).
    QByteArray line = decorated ? QByteArrayLiteral("\r\n\x1b[2m--- ") : QByteArrayLiteral("\x1b[2m");
    line += encodeForDevice(text.toUtf8());
    line += decorated ? QByteArrayLiteral(" ---\x1b[0m\r\n") : QByteArrayLiteral("\x1b[0m\r\n");
    m_terminal->feedData(line);
}

QString SessionWidget::sanitizeForFileName(const QString& name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        const bool keep = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') ||
                          u == u'.' || u == u'_' || u == u'@' || u == u'-';
        out += keep ? c : QLatin1Char('_');
    }
    return out;
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

QString SessionWidget::persistentStatusMessage() const
{
    if (m_terminal->isOutputPaused()) {
        return tr("Output paused while selecting - Enter copies, Esc cancels");
    }
    return {};
}

void SessionWidget::replayLogFile(const QString& path, qint64 bytesPerSecond)
{
    if (m_transport->state() != Transport::State::Disconnected) {
        // Replayed bytes would interleave with live device output; the user disconnects first.
        emit statusMessage(tr("Disconnect from %1 before replaying a log file").arg(m_transport->displayName()),
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

#undef SU_LC
