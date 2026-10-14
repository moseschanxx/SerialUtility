#include "firmware/ToolRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "app/Logging.h"

// ---------------------------------------------------------------------------------------------
// ToolResult / ToolInvocation base
// ---------------------------------------------------------------------------------------------

QString ToolResult::outputText() const
{
    QString text = QString::fromUtf8(output);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

ToolInvocation::ToolInvocation(QObject* parent) : QObject(parent) {}
ToolInvocation::~ToolInvocation() = default;

void ToolInvocation::markStarted()
{
    // started() once, and never after the end (a queued "started" can lose the race against a
    // process that ends at once).
    if (m_finished || m_result.started) {
        return;
    }
    m_running = true;
    m_result.started = true;
    emit started();
}

void ToolInvocation::appendOutput(const QByteArray& chunk)
{
    if (m_finished || chunk.isEmpty()) {
        return;
    }
    m_result.output.append(chunk);
    emit outputReceived(chunk);
}

void ToolInvocation::finish(const ToolResult& result)
{
    if (m_finished) {
        return;   // finished() is emitted exactly once, whoever tries to end the command
    }
    ToolResult ended = result;
    if (ended.commandLine.isEmpty()) {
        ended.commandLine = m_result.commandLine;
    }
    if (ended.output.isEmpty()) {
        ended.output = m_result.output;   // implementations may hand over a bare result
    }
    m_result = ended;
    m_running = false;
    m_finished = true;

    // A slot may delete this object (or its owner) while it handles finished(): emit a copy and
    // look at `this` only through a guard afterwards. The copy shares its buffers (COW).
    const QPointer<ToolInvocation> self(this);
    const ToolResult copy = m_result;
    emit finished(copy);
    if (self && m_autoDelete) {
        deleteLater();
    }
}

// ---------------------------------------------------------------------------------------------
// ToolRunner
// ---------------------------------------------------------------------------------------------

ToolRunner::ToolRunner(QObject* parent) : QObject(parent) {}
ToolRunner::~ToolRunner() = default;

namespace {

/// One argument as it appears in a log line: quoted when it is empty or holds white space or a
/// double quote (the quote is escaped), otherwise as is. For reading only - nothing parses it.
QString quoteForLog(const QString& argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    const bool needsQuotes = argument.contains(QLatin1Char(' ')) || argument.contains(QLatin1Char('\t'))
                             || argument.contains(QLatin1Char('"'));
    if (!needsQuotes) {
        return argument;
    }
    QString escaped = argument;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

} // namespace

QString ToolRunner::formatCommandLine(const QString& program, const QStringList& arguments)
{
    QStringList parts;
    parts.reserve(arguments.size() + 1);
    // The program is quoted like an argument: "C:/Program Files/adb.exe" must stay one word.
    parts << quoteForLog(program);
    for (const QString& argument : arguments) {
        parts << quoteForLog(argument);
    }
    return parts.join(QLatin1Char(' '));
}

// ---------------------------------------------------------------------------------------------
// ProcessToolRunner
// ---------------------------------------------------------------------------------------------

namespace {

constexpr int kCancelGraceMs = 2000;   ///< terminate() -> kill() (header: "kill() after 2 s")

/**
 * One QProcess behind the ToolInvocation interface. Everything the callers can observe is
 * delivered from the event loop:
 *  - the process is started inside run() (so the command is under way when run() returns), but
 *    started() is queued, and a program that cannot be started is reported by a queued finish();
 *  - output arrives through QProcess::readyRead, finished() follows the last chunk.
 * The invocation is "running" from construction until finished(), which is what
 * FakeToolRunner::liveInvocations() and the owners' bookkeeping want to see.
 */
class ProcessInvocation : public ToolInvocation
{
public:
    ProcessInvocation(const QString& program, const QStringList& arguments,
                      const ToolRunner::Options& options, QObject* parent)
        : ToolInvocation(parent), m_program(program), m_arguments(arguments), m_options(options)
    {
        m_result.commandLine = ToolRunner::formatCommandLine(program, arguments);
        m_running = true;
    }

    ~ProcessInvocation() override
    {
        // An owner that deletes a running invocation (the tool runner at shutdown) must not leave
        // the child behind or get Qt's "destroyed while process is still running" warning.
        if (m_process && m_process->state() != QProcess::NotRunning) {
            m_process->disconnect(this);
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }

    void begin()
    {
        if (m_options.detached) {
            beginDetached();
            return;
        }
        qCInfo(lcFirmware).noquote() << "run:" << m_result.commandLine;

        m_process = new QProcess(this);
        m_process->setProcessChannelMode(QProcess::MergedChannels);
        configure(m_process);

        connect(m_process, &QProcess::readyRead, this, [this] { drain(); });
        connect(m_process, &QProcess::started, this, [this] {
            QTimer::singleShot(0, this, [this] { announceStarted(); });
            if (m_cancelRequested) {
                m_process->kill();   // cancel() arrived while the process was still starting
                return;
            }
            // Input handed over while the process was still Starting (on Linux the state stays
            // Starting until the child has exec'ed; Windows is Running right after start()).
            if (!m_pendingInput.isEmpty()) {
                m_process->write(m_pendingInput);
                m_pendingInput.clear();
            }
            if (m_closeInputAfterStart) {
                m_process->closeWriteChannel();
            }
        });
        connect(m_process, &QProcess::finished, this,
                [this](int exitCode, QProcess::ExitStatus status) { onProcessFinished(exitCode, status); });
        connect(m_process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) { onProcessError(error); });

        m_process->start(m_program, m_arguments);

        if (m_options.timeoutMs > 0) {
            QTimer::singleShot(m_options.timeoutMs, this, [this] { onTimeout(); });
        }
    }

    void writeInput(const QByteArray& data) override
    {
        if (!m_process || m_finished) {
            return;
        }
        if (m_process->state() == QProcess::Starting) {
            m_pendingInput.append(data);   // delivered from the started() handler
        } else if (m_process->state() == QProcess::Running) {
            m_process->write(data);
        }
    }

    void closeInput() override
    {
        if (!m_process || m_finished) {
            return;
        }
        if (m_process->state() == QProcess::Starting) {
            m_closeInputAfterStart = true;
        } else if (m_process->state() == QProcess::Running) {
            m_process->closeWriteChannel();
        }
    }

    void cancel() override
    {
        if (m_finished || m_cancelRequested) {
            return;   // finished: nothing to cancel; twice: the first one is under way
        }
        m_cancelRequested = true;
        if (!m_process || m_process->state() == QProcess::NotRunning) {
            return;   // a failed start is already queued: its finish() reports cancelled
        }
        if (m_process->state() == QProcess::Starting) {
            return;   // killed from the started() handler
        }
#ifdef Q_OS_WIN
        // terminate() posts WM_CLOSE, which a console program (adb, upgrade_tool) never sees:
        // asking first would only add the 2 s grace period to every cancel.
        m_process->kill();
#else
        m_process->terminate();
        QTimer::singleShot(kCancelGraceMs, this, [this] {
            if (!m_finished && m_process->state() != QProcess::NotRunning) {
                m_process->kill();
            }
        });
#endif
    }

private:
    /// Working directory and environment of Options, for a process started either way, and the
    /// child's separation from whatever reaches its caller (v0.6.0). upgrade_tool ended in the
    /// middle of a write leaves a board that does not boot, so nothing aimed at the caller may end
    /// it: on Windows the tool gets no console of the caller's (CREATE_NO_WINDOW: a hidden console of
    /// its own), so Ctrl+C, Ctrl+Break and closing the console window of su-fw never reach it -
    /// exactly how the GUI, which has no console, always ran the tools (Qt sets the flag itself
    /// then). On POSIX the tool leads a process group of its own, so the terminal's Ctrl+C and
    /// Ctrl+backslash and a signal to su-fw's group are not delivered to it, and a write to a pipe
    /// whose reader is gone fails instead of raising SIGPIPE (su-fw killed while upgrade_tool still
    /// writes). The output still comes through the pipes either way. Only a kill of the whole
    /// process tree (or of a job object) still takes the tool along.
    void configure(QProcess* process) const
    {
#ifdef Q_OS_WIN
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#else
        process->setChildProcessModifier([] { ::setpgid(0, 0); });
        process->setUnixProcessParameters(QProcess::UnixProcessFlags(QProcess::UnixProcessFlag::IgnoreSigPipe));
#endif
        if (!m_options.workingDirectory.isEmpty()) {
            process->setWorkingDirectory(m_options.workingDirectory);
        }
        if (!m_options.environment.isEmpty()) {
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            for (const QString& entry : std::as_const(m_options.environment)) {
                // "NAME=value"; a leading '=' belongs to the name (Windows' "=C:" variables).
                const qsizetype eq = entry.indexOf(QLatin1Char('='), 1);
                if (eq < 1) {
                    qCWarning(lcFirmware) << "ignoring environment entry without '=':" << entry;
                    continue;
                }
                environment.insert(entry.left(eq), entry.mid(eq + 1));
            }
            process->setProcessEnvironment(environment);
        }
    }

    /// Options::detached: QProcess::startDetached() owns nothing afterwards, so neither this
    /// invocation's destructor nor the runner's can kill the command (McuLink's cleanup at
    /// shutdown, which must reach the board after the application is gone). Its output goes
    /// nowhere; the result only says whether it started.
    void beginDetached()
    {
        qCInfo(lcFirmware).noquote() << "run detached:" << m_result.commandLine;
        QProcess process;
        process.setProgram(m_program);
        process.setArguments(m_arguments);
        configure(&process);
        process.setStandardInputFile(QProcess::nullDevice());
        process.setStandardOutputFile(QProcess::nullDevice());
        process.setStandardErrorFile(QProcess::nullDevice());
        qint64 pid = 0;
        const bool started = process.startDetached(&pid);
        const QString reason = started ? QString() : process.errorString();
        if (!started) {
            qCWarning(lcFirmware).noquote() << "cannot start:" << m_result.commandLine << "-" << reason;
        }
        QTimer::singleShot(0, this, [this, started, reason] {
            if (started) {
                announceStarted();
            }
            ToolResult result = m_result;
            result.started = started;
            result.exitCode = started ? 0 : -1;
            result.error = reason;
            finish(result);
        });
    }

    void announceStarted()
    {
        markStarted();   // no-op when already announced or finished
    }

    void drain()
    {
        if (!m_process || m_finished) {
            return;
        }
        const QByteArray chunk = m_process->readAll();
        if (!chunk.isEmpty()) {
            announceStarted();   // started() always precedes the first output
            appendOutput(chunk);
        }
    }

    void onTimeout()
    {
        if (m_finished || !m_process || m_process->state() == QProcess::NotRunning) {
            return;
        }
        qCWarning(lcFirmware).noquote() << "timed out after" << m_options.timeoutMs << "ms:" << m_result.commandLine;
        m_timedOut = true;
        m_process->kill();
    }

    void onProcessFinished(int exitCode, QProcess::ExitStatus status)
    {
        drain();
        announceStarted();
        ToolResult result = m_result;
        result.started = true;
        result.cancelled = m_cancelRequested;
        result.timedOut = m_timedOut && !m_cancelRequested;
        result.exitCode = exitCode;
        if (result.cancelled) {
            result.exitCode = -1;
        } else if (result.timedOut) {
            result.exitCode = -1;
            result.error = QCoreApplication::translate("ToolRunner", "timed out after %1 ms").arg(m_options.timeoutMs);
        } else if (status == QProcess::CrashExit) {
            result.exitCode = -1;
            result.error = QCoreApplication::translate("ToolRunner", "the process crashed");
        }
        qCInfo(lcFirmware).noquote() << "done:" << m_result.commandLine << "exit" << result.exitCode
                                     << (result.cancelled ? "cancelled" : result.timedOut ? "timed out" : "");
        finish(result);
    }

    void onProcessError(QProcess::ProcessError error)
    {
        if (error != QProcess::FailedToStart) {
            // Crashed arrives again as finished(); the others are not fatal on their own.
            qCDebug(lcFirmware) << "process error" << error << m_process->errorString();
            return;
        }
        // Qt may report this from inside start(): finish from the event loop, as run() promises.
        const QString reason = m_process->errorString();
        qCWarning(lcFirmware).noquote() << "cannot start:" << m_result.commandLine << "-" << reason;
        QTimer::singleShot(0, this, [this, reason] {
            ToolResult result = m_result;
            result.started = false;
            result.cancelled = m_cancelRequested;
            result.exitCode = -1;
            result.error = reason;
            finish(result);
        });
    }

    QString m_program;
    QStringList m_arguments;
    ToolRunner::Options m_options;
    QProcess* m_process = nullptr;
    QByteArray m_pendingInput;          ///< writeInput() while Starting (see the started() handler)
    bool m_closeInputAfterStart = false; ///< closeInput() while Starting
    bool m_cancelRequested = false;
    bool m_timedOut = false;
};

} // namespace

ProcessToolRunner::ProcessToolRunner(QObject* parent) : ToolRunner(parent) {}

ToolInvocation* ProcessToolRunner::run(const QString& program, const QStringList& arguments, const Options& options)
{
    auto* invocation = new ProcessInvocation(program, arguments, options, this);
    invocation->begin();
    return invocation;
}

// ---------------------------------------------------------------------------------------------
// ToolLocator
// ---------------------------------------------------------------------------------------------

namespace ToolLocator {

namespace {

enum class Source { None, Preferences, Bundled, Path };

struct Resolved
{
    QString path;
    Source source = Source::None;
};

bool isFile(const QString& path)
{
    return !path.isEmpty() && QFileInfo(path).isFile();
}

Resolved resolve(Tool tool, const QString& override)
{
    Resolved resolved;
    if (isFile(override)) {
        resolved.path = QFileInfo(override).absoluteFilePath();
        resolved.source = Source::Preferences;
        return resolved;
    }
    const QString bundled = bundledPath(tool);
    if (isFile(bundled)) {
        resolved.path = QFileInfo(bundled).absoluteFilePath();
        resolved.source = Source::Bundled;
        return resolved;
    }
    if (tool == Tool::Adb) {
        // "prefer a user's newer adb on PATH only if the bundled one is missing" (spec, Packaging)
        const QString onPath = QStandardPaths::findExecutable(QStringLiteral("adb"));
        if (!onPath.isEmpty()) {
            resolved.path = QDir::fromNativeSeparators(onPath);
            resolved.source = Source::Path;
        }
    }
    return resolved;
}

} // namespace

QString bundledToolsDirectory()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/tools");
}

QString fileName(Tool tool)
{
#ifdef Q_OS_WIN
    switch (tool) {
    case Tool::UpgradeTool: return QStringLiteral("upgrade_tool.exe");
    case Tool::Adb: return QStringLiteral("adb.exe");
    case Tool::DriverInstaller: return QStringLiteral("DriverAssistant/DriverInstall.exe");
    }
#else
    switch (tool) {
    case Tool::UpgradeTool: return QStringLiteral("upgrade_tool");
    case Tool::Adb: return QStringLiteral("adb");
    // Windows only; the same name keeps bundledPath() total, the file simply never exists.
    case Tool::DriverInstaller: return QStringLiteral("DriverAssistant/DriverInstall.exe");
    }
#endif
    return QString();
}

QString bundledPath(Tool tool)
{
    return bundledToolsDirectory() + QLatin1Char('/') + fileName(tool);
}

QString locate(Tool tool, const QString& override)
{
    return resolve(tool, override).path;
}

QString describe(Tool tool, const QString& override)
{
    const Resolved resolved = resolve(tool, override);
    switch (resolved.source) {
    case Source::Preferences: return QStringLiteral("preferences ") + resolved.path;
    case Source::Bundled: return QStringLiteral("bundled ") + resolved.path;
    case Source::Path: return QStringLiteral("PATH ") + resolved.path;
    case Source::None: break;
    }
    return QStringLiteral("not found");
}

} // namespace ToolLocator
