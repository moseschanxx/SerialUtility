#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QTabWidget>
#include <QTimer>

#include "Version.h"
#include "app/Logging.h"
#include "core/SerialConnection.h"
#include "ui/ConnectionBar.h"
#include "ui/MainWindow.h"
#include "ui/SessionWidget.h"
#include "ui/SystemLogViewer.h"

namespace {

/// --baud <rate|auto> for a serial session: select that fixed rate - or the bar's "Auto" item,
/// which detects the rate on connect - before the port is opened. Ignored, with a warning, on an
/// SSH session and for a value outside SerialSettings' baud range.
void applyBaudArgument(SessionWidget* session, const QString& value)
{
    if (session->isSsh()) {
        qCWarning(lcApp) << "--baud is ignored for an SSH target";
        return;
    }
    ConnectionBar* bar = session->connectionBar();
    SerialSettings settings = bar->settings();
    const QString text = value.trimmed();
    if (text.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        settings.autoBaud = true;   // baudRate stays the bar's starting rate
    } else {
        bool ok = false;
        const qint32 baud = text.toInt(&ok);
        if (!ok || !SerialSettings::isValidBaudRate(baud)) {
            qCWarning(lcApp) << "invalid --baud" << value << "- expected a rate between" << SerialSettings::kMinBaudRate
                             << "and" << SerialSettings::kMaxBaudRate << "or \"auto\"; ignored";
            return;
        }
        settings.autoBaud = false;
        settings.baudRate = baud;
    }
    bar->setSettings(settings);
}

/// Make `port` (a serial port name or an SSH restore key) the current session: reuse a tab that
/// already shows it, otherwise fill the current empty tab of the same kind, otherwise open a new
/// one. Returns the session (never null).
SessionWidget* selectPortSession(MainWindow& window, const QString& port)
{
    const bool ssh = SessionWidget::isSshRestoreKey(port);
    for (int i = 0; i < window.sessionCount(); ++i) {
        SessionWidget* session = window.sessionAt(i);
        if (session && session->portName() == port) {
            // The tab widget is part of MainWindow.ui's contract ("tabWidget").
            if (auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("tabWidget"))) {
                tabs->setCurrentIndex(i);
            }
            return session;
        }
    }
    if (SessionWidget* current = window.currentSession();
        current && current->portName().isEmpty() && current->isSsh() == ssh) {
        if (ssh) {
            current->setSshTarget(port);
        } else {
            current->setPortName(port);
        }
        return current;
    }
    return window.newSession(port);   // dispatches to newSshSession() for an SSH key
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    // Install first so Qt's own start-up warnings and everything MainWindow's constructor logs
    // (quick-command file problems, font spec, translations, enumerator) reach the System Log
    // dock: the handler buffers messages until the dock's viewer registers itself.
    SystemLogViewer::installMessageHandler();
    QCoreApplication::setOrganizationName(QStringLiteral(APP_ORGANIZATION));
    QCoreApplication::setOrganizationDomain(QStringLiteral(APP_ORGANIZATION_DOMAIN));
    QCoreApplication::setApplicationName(QStringLiteral(APP_NAME));
    // Deliberately no setApplicationDisplayName(): the Windows platform plugin appends the
    // display name to every window title that does not end with it, which turned the
    // MainWindow title "BuildAI Serial Utility 0.1.0 - COM8" into
    // "... - BuildAI Serial Utility". MainWindow::updateWindowTitle() owns the title format.
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/buildai.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("BuildAI Serial Utility - serial-port terminal for Rockchip Linux boards and MCU shells"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("port"),
                                 QStringLiteral("Serial port to pre-select in the first tab (e.g. COM8)."),
                                 QStringLiteral("[port]"));
    const QCommandLineOption connectOption({QStringLiteral("c"), QStringLiteral("connect")},
                                           QStringLiteral("Open the given port or SSH target immediately."));
    parser.addOption(connectOption);
    const QCommandLineOption baudOption(
        QStringLiteral("baud"),
        QStringLiteral("Baud rate for the serial port (a number, or \"auto\" to detect it on connect); used with a port name."),
        QStringLiteral("rate"));
    parser.addOption(baudOption);
    const QCommandLineOption sshOption(
        QStringLiteral("ssh"),
        QStringLiteral("Open an SSH session tab for user@host[:port] (or ssh://...) instead of a serial port."),
        QStringLiteral("target"));
    parser.addOption(sshOption);
    const QCommandLineOption replayOption(
        {QStringLiteral("r"), QStringLiteral("replay")},
        QStringLiteral("Replay a captured log file (raw or timestamped text capture) into the first tab."),
        QStringLiteral("file"));
    parser.addOption(replayOption);
    const QCommandLineOption speedOption(
        QStringLiteral("speed"),
        QStringLiteral("Replay speed in baud (default 115200; 0 = as fast as possible). Used with --replay."),
        QStringLiteral("baud"));
    parser.addOption(speedOption);
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    QString portArg = positional.isEmpty() ? QString() : positional.first().trimmed();
    const bool connectNow = parser.isSet(connectOption);
    if (parser.isSet(sshOption)) {
        // "user@host:port" -> the SSH restore key selectPortSession() understands.
        const QString target = parser.value(sshOption).trimmed();
        portArg = SessionWidget::isSshRestoreKey(target) ? target : QStringLiteral("ssh:target:") + target;
    }

    MainWindow window;

    qCInfo(lcApp) << "BuildAI Serial Utility" << APP_VERSION << APP_GIT_HASH << "Qt" << qVersion();

    if (!portArg.isEmpty()) {
        SessionWidget* session = selectPortSession(window, portArg);
        if (parser.isSet(baudOption)) {
            applyBaudArgument(session, parser.value(baudOption));
        }
        if (connectNow) {
            // Connect once the event loop runs so status messages land in the visible window.
            QTimer::singleShot(0, session, [session]() { session->connectPort(); });
        }
    } else {
        if (connectNow) {
            qCWarning(lcApp) << "--connect given without a port name or --ssh target; ignored";
        }
        if (parser.isSet(baudOption)) {
            qCWarning(lcApp) << "--baud given without a port name; ignored";
        }
    }

    if (parser.isSet(replayOption)) {
        const QString replayFile = parser.value(replayOption);
        qint64 bytesPerSecond = 11520;   // 115200 baud
        if (parser.isSet(speedOption)) {
            bool ok = false;
            const qint64 baud = parser.value(speedOption).toLongLong(&ok);
            if (ok && baud >= 0) {
                bytesPerSecond = baud / 10;
            } else {
                qCWarning(lcApp) << "invalid --speed" << parser.value(speedOption) << "- using 115200 baud";
            }
        }
        SessionWidget* session = window.currentSession() ? window.currentSession() : window.newSession();
        QTimer::singleShot(0, session, [session, replayFile, bytesPerSecond]() {
            session->replayLogFile(replayFile, bytesPerSecond);
        });
    }

    window.show();
    return QApplication::exec();
}
