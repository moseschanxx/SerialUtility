#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

/**
 * The result of one external tool command (upgrade_tool, adb) once it finished.
 */
struct ToolResult
{
    bool started = false;       ///< false: the program could not be started (error says why)
    bool cancelled = false;     ///< ToolInvocation::cancel() ended it
    bool timedOut = false;      ///< ToolRunner::Options::timeoutMs ended it
    int exitCode = -1;          ///< -1 when it did not start, crashed, was cancelled or timed out
    QByteArray output;          ///< stdout and stderr merged, raw (carriage returns kept)
    QString error;              ///< why it failed to start / crashed / timed out, empty otherwise
    QString commandLine;        ///< for log lines: "upgrade_tool.exe UF C:/.../update.img"

    bool succeeded() const { return started && !cancelled && !timedOut && exitCode == 0; }
    QString outputText() const;   ///< output as UTF-8 text with "\r\n" and "\r" turned into "\n"
};

/**
 * One running (or finished) external command. Created by ToolRunner::run(); parented to the
 * runner. finished(result) is emitted exactly once, after the last outputReceived(); then the
 * object is scheduled for deletion with deleteLater() unless setAutoDelete(false) was called, so a
 * handler of finished() may read result() during the signal but must not keep the pointer.
 * cancel() on a running command terminates it (QProcess::terminate(), then kill() after 2 s) and
 * reports result().cancelled = true; cancel() on a finished one is a no-op.
 * writeInput() / closeInput() feed the command's stdin (unused by upgrade_tool / adb so far).
 */
class ToolInvocation : public QObject
{
    Q_OBJECT
public:
    explicit ToolInvocation(QObject* parent = nullptr);
    ~ToolInvocation() override;

    QString commandLine() const { return m_result.commandLine; }
    bool isRunning() const { return m_running; }
    bool isFinished() const { return m_finished; }
    const ToolResult& result() const { return m_result; }
    QByteArray output() const { return m_result.output; }   ///< what arrived so far
    void setAutoDelete(bool on) { m_autoDelete = on; }
    bool autoDelete() const { return m_autoDelete; }

public slots:
    virtual void writeInput(const QByteArray& data) = 0;
    virtual void closeInput() = 0;
    virtual void cancel() = 0;

signals:
    void started();
    void outputReceived(const QByteArray& chunk);   ///< also appended to result().output
    void finished(const ToolResult& result);

protected:
    /// Implementations call these: appendOutput() emits outputReceived(); finish() marks the
    /// result, emits finished() once and schedules the deletion.
    void markStarted();
    void appendOutput(const QByteArray& chunk);
    void finish(const ToolResult& result);

    ToolResult m_result;
    bool m_running = false;
    bool m_finished = false;
    bool m_autoDelete = true;
};

/**
 * Starts external tools asynchronously (never blocks the GUI thread). ProcessToolRunner runs real
 * processes with QProcess; the tests use FakeToolRunner (tests/support/FakeToolRunner.h) with
 * scripted outputs. Every firmware class takes a ToolRunner* (not owned) so the fakes slot in.
 */
class ToolRunner : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString workingDirectory;     ///< empty = the parent process's
        int timeoutMs = 0;            ///< > 0: the command is killed after this time (result.timedOut)
        QStringList environment;      ///< "NAME=value" entries added to the process environment
    };

    explicit ToolRunner(QObject* parent = nullptr);
    ~ToolRunner() override;

    /// Start `program` with `arguments`; the invocation is returned at once (never null) and
    /// reports through its signals - a program that cannot be started reports finished() with
    /// started = false from the event loop, never from inside run().
    virtual ToolInvocation* run(const QString& program, const QStringList& arguments,
                                const Options& options) = 0;
    /// The same with default Options (a separate overload: GCC refuses `= Options()` as a
    /// default argument inside the class that defines Options; subclasses say `using ToolRunner::run;`).
    ToolInvocation* run(const QString& program, const QStringList& arguments);

    /// "prog arg1 \"arg with space\"" for log lines (QProcess-style quoting).
    static QString formatCommandLine(const QString& program, const QStringList& arguments);
};

inline ToolInvocation* ToolRunner::run(const QString& program, const QStringList& arguments)
{
    return run(program, arguments, Options());
}

/// The real thing: QProcess with merged channels (QProcess::MergedChannels).
class ProcessToolRunner : public ToolRunner
{
    Q_OBJECT
public:
    explicit ProcessToolRunner(QObject* parent = nullptr);
    using ToolRunner::run;
    ToolInvocation* run(const QString& program, const QStringList& arguments,
                        const Options& options) override;
};

/**
 * Where the external tools are (spec "Packaging"): the Preferences override when set and
 * existing, else the bundled copy in `<application dir>/tools/` (upgrade_tool.exe / upgrade_tool,
 * adb.exe / adb, DriverAssistant/DriverInstall.exe), else - adb only - the first `adb` on PATH
 * ("prefer a user's newer adb on PATH only if the bundled one is missing"). An empty result means
 * "not found"; describe() says which one was taken for the log.
 */
namespace ToolLocator {

enum class Tool { UpgradeTool, Adb, DriverInstaller };

QString bundledToolsDirectory();                       ///< <applicationDirPath>/tools (may not exist)
QString bundledPath(Tool tool);                        ///< the file inside bundledToolsDirectory(), existing or not
QString locate(Tool tool, const QString& override = QString());
/// "bundled C:/.../tools/adb.exe", "PATH C:/.../adb.exe", "preferences D:/x/adb.exe", "not found".
QString describe(Tool tool, const QString& override = QString());
QString fileName(Tool tool);                           ///< platform file name ("upgrade_tool.exe" / "upgrade_tool")

} // namespace ToolLocator
