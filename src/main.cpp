#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QIcon>
#include <QTabWidget>
#include <QTimer>

#include "Version.h"
#include "app/Logging.h"
#include "ui/MainWindow.h"
#include "ui/SessionWidget.h"
#include "ui/SystemLogViewer.h"

namespace {

/// Make `port` the current session: reuse a tab that already shows it, otherwise fill the
/// current empty tab, otherwise open a new one. Returns the session (never null).
SessionWidget* selectPortSession(MainWindow& window, const QString& port)
{
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
    if (SessionWidget* current = window.currentSession(); current && current->portName().isEmpty()) {
        current->setPortName(port);
        return current;
    }
    return window.newSession(port);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
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
                                           QStringLiteral("Open the given port immediately."));
    parser.addOption(connectOption);
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
    const QString portArg = positional.isEmpty() ? QString() : positional.first().trimmed();
    const bool connectNow = parser.isSet(connectOption);

    MainWindow window;
    SystemLogViewer::installMessageHandler();

    qCInfo(lcApp) << "BuildAI Serial Utility" << APP_VERSION << APP_GIT_HASH << "Qt" << qVersion();

    if (!portArg.isEmpty()) {
        SessionWidget* session = selectPortSession(window, portArg);
        if (connectNow) {
            // Connect once the event loop runs so status messages land in the visible window.
            QTimer::singleShot(0, session, [session]() { session->connectPort(); });
        }
    } else if (connectNow) {
        qCWarning(lcApp) << "--connect given without a port name; ignored";
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
