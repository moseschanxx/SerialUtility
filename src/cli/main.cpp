// su-fw: the Firmware tab's operations for a command shell (v0.6.0; docs/firmware-update/CLI_PLAN.md).
// A console program of its own because the GUI executable is a Windows-subsystem program that cannot
// write to the shell that started it. Everything it does is FirmwareCli (su_core); this file is the
// process around it: QCoreApplication with the app's names (so the settings, SSH profiles and saved
// passwords are the app's), stdout / stderr / stdin, the terminal's echo for a typed password, and
// Ctrl+C.
#include <QCoreApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QString>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "Version.h"
#include "firmware/FirmwareCli.h"
#include "firmware/ToolRunner.h"

namespace {

volatile std::sig_atomic_t g_interrupts = 0;

void onInterrupt(int signalNumber)
{
    g_interrupts = g_interrupts + 1;
    std::signal(signalNumber, onInterrupt);   // the Microsoft runtime resets the handler before calling it
}

bool stdinIsTerminal()
{
#ifdef Q_OS_WIN
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

void setTerminalEcho(bool on)
{
#ifdef Q_OS_WIN
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        return;
    }
    SetConsoleMode(input, on ? (mode | ENABLE_ECHO_INPUT) : (mode & ~DWORD(ENABLE_ECHO_INPUT)));
#else
    termios settings{};
    if (tcgetattr(STDIN_FILENO, &settings) != 0) {
        return;
    }
    if (on) {
        settings.c_lflag |= ECHO;
    } else {
        settings.c_lflag &= ~tcflag_t(ECHO);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &settings);
#endif
}

/// One line from stdin; a secret is read without echo on a terminal.
std::optional<QString> readInputLine(bool secret)
{
    const bool terminal = stdinIsTerminal();
    if (secret && terminal) {
        setTerminalEcho(false);
    }
    std::string line;
    const bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (secret && terminal) {
        setTerminalEcho(true);
        std::fputs("\n", stderr);   // the Enter that was not echoed
        std::fflush(stderr);
    }
    if (!ok) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    // A pipe from a script or an agent is UTF-8; a terminal types in the local code page.
    return terminal ? QString::fromLocal8Bit(line.data(), qsizetype(line.size())) : QString::fromUtf8(line.data(), qsizetype(line.size()));
}

/// Ctrl+C reaches every process attached to the terminal (Windows: the console), upgrade_tool and
/// adb included - and upgrade_tool killed while it writes the flash leaves a board that does not
/// boot. A child inherits "Ctrl+C is ignored" from its parent when it is created (Windows:
/// SetConsoleCtrlHandler(nullptr, TRUE); POSIX: SIG_IGN survives exec), so su-fw ignores it for the
/// moment a tool is started (QProcess::start() creates the process inside run()); su-fw itself stops
/// the tools it may stop through the job (FirmwareJob::cancel(): only while waiting).
class ShieldedToolRunner : public ProcessToolRunner
{
public:
    using ToolRunner::run;
    ToolInvocation* run(const QString& program, const QStringList& arguments, const Options& options) override
    {
#ifdef Q_OS_WIN
        SetConsoleCtrlHandler(nullptr, TRUE);
        ToolInvocation* invocation = ProcessToolRunner::run(program, arguments, options);
        SetConsoleCtrlHandler(nullptr, FALSE);
#else
        const auto previous = std::signal(SIGINT, SIG_IGN);
        ToolInvocation* invocation = ProcessToolRunner::run(program, arguments, options);
        std::signal(SIGINT, previous);
#endif
        return invocation;
    }
};

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    // The app's names: AppSettings, the SSH profiles and SecretStore are the GUI's.
    QCoreApplication::setOrganizationName(QStringLiteral(APP_ORGANIZATION));
    QCoreApplication::setOrganizationDomain(QStringLiteral(APP_ORGANIZATION_DOMAIN));
    QCoreApplication::setApplicationName(QStringLiteral(APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    // The job mirrors every log line to the "buildai.firmware" category for the GUI's System Log,
    // and the SSH stack warns about a refused connect that the job reports anyway: on a console
    // that would repeat lines on stderr, which stays for usage errors. QT_LOGGING_RULES (e.g.
    // "buildai.*=true") still turns them on for a diagnosis; Qt's own messages are untouched.
    QLoggingCategory::setFilterRules(QStringLiteral("buildai.*.debug=false\nbuildai.*.info=false\nbuildai.*.warning=false"));

#ifdef Q_OS_WIN
    const UINT previousCodePage = GetConsoleOutputCP();
    if (previousCodePage != 0) {
        SetConsoleOutputCP(CP_UTF8);   // the output is UTF-8 (paths, the board's answers)
    }
#endif

    QFile out;
    QFile err;
    if (!out.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered) || !err.open(stderr, QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        return FirmwareCli::ExitFailed;
    }

    ShieldedToolRunner runner;
    FirmwareCli cli(&runner);
    cli.setInput(&readInputLine, stdinIsTerminal());

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onInterrupt);   // Ctrl+Break on Windows
#endif
    // The handler only counts (it may run on another thread); the event loop passes it on.
    int handled = 0;
    QTimer interrupts;
    QObject::connect(&interrupts, &QTimer::timeout, &cli, [&handled, &cli]() {
        while (handled < int(g_interrupts)) {
            ++handled;
            cli.interrupt();
        }
    });
    interrupts.start(100);

    const int code = cli.run(QCoreApplication::arguments().mid(1), &out, &err);

#ifdef Q_OS_WIN
    if (previousCodePage != 0) {
        SetConsoleOutputCP(previousCodePage);
    }
#endif
    return code;
}
