#pragma once

#include <QObject>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

#include "firmware/ToolRunner.h"

class FakeToolRunner;

/**
 * A scripted ToolInvocation: the test (or a FakeToolRunner handler) drives it with emitOutput()
 * and finish(), or lets it hang until cancel(). Everything is delivered from the event loop
 * (QTimer::singleShot with the given delay), never from inside run(), like the real process.
 */
class FakeToolInvocation : public ToolInvocation
{
    Q_OBJECT
public:
    explicit FakeToolInvocation(const QString& program, const QStringList& arguments, QObject* parent = nullptr);

    QString program() const { return m_program; }
    QStringList arguments() const { return m_arguments; }
    QByteArray inputReceived() const { return m_input; }   ///< what writeInput() delivered
    bool inputClosed() const { return m_inputClosed; }

    /// Deliver `chunk` after `delayMs` (queued in order with the other deliveries).
    void emitOutput(const QByteArray& chunk, int delayMs = 0);
    /// End with `exitCode` after `delayMs` (after any output queued before it).
    void finish(int exitCode, int delayMs = 0);
    /// End as "could not start" with `message`.
    void failToStart(const QString& message);
    /// (v0.7.0) End as "timed out" (what ToolRunner::Options::timeoutMs does to a real process;
    /// the fake runner applies no timeout of its own) after `delayMs`.
    void timeOut(int delayMs = 0);
    /// Mark as started without ending it; it ends only through cancel() (or a later finish()).
    void hang();

    void writeInput(const QByteArray& data) override;
    void closeInput() override;
    void cancel() override;   ///< result.cancelled, exit code -1, finished() from the event loop

private:
    void schedule(int delayMs, std::function<void()> step);
    QString m_program;
    QStringList m_arguments;
    QByteArray m_input;
    bool m_inputClosed = false;
    int m_pendingDelayMs = 0;   ///< deliveries are serialized: each waits for the previous one
};

/**
 * ToolRunner for the tests: no process is ever started. run() records the command, creates a
 * FakeToolInvocation and offers it to the handlers in registration order; the first handler
 * whose pattern matches the command line (ToolRunner::formatCommandLine(program, args)) drives
 * it; a command no handler takes is finished with exit code 127 and the output
 * "fake: no handler for <command line>" so a test fails loudly.
 *
 * Shortcuts: addScript(pattern, output, exitCode, delayMs) answers with a fixed output;
 * addHandler(pattern, fn) hands the invocation to a function (hang it, answer according to the
 * arguments, start a FakeMcuServer, ...). invocations() lists every command line run so far,
 * optionsAt(i) the Options it was run with (timeout, working directory, detached), count(pattern)
 * how many matched; reset() clears handlers and the record.
 */
class FakeToolRunner : public ToolRunner
{
    Q_OBJECT
public:
    using Handler = std::function<void(FakeToolInvocation* invocation)>;

    explicit FakeToolRunner(QObject* parent = nullptr);

    using ToolRunner::run;
    ToolInvocation* run(const QString& program, const QStringList& arguments,
                        const Options& options) override;

    void addScript(const QRegularExpression& commandLine, const QByteArray& output, int exitCode = 0, int delayMs = 0);
    void addHandler(const QRegularExpression& commandLine, Handler handler);
    void reset();

    QStringList invocations() const { return m_invocations; }          ///< command lines, in order
    QList<FakeToolInvocation*> liveInvocations() const;                ///< still running (not auto-deleted yet)
    int count(const QRegularExpression& commandLine) const;
    QString lastInvocation() const { return m_invocations.isEmpty() ? QString() : m_invocations.last(); }
    Options lastOptions() const { return m_lastOptions; }
    /// The Options of invocations().at(index) (default Options for an index out of range).
    Options optionsAt(int index) const { return index >= 0 && index < m_options.size() ? m_options.at(index) : Options(); }

signals:
    void invocationStarted(FakeToolInvocation* invocation);

private:
    struct Entry
    {
        QRegularExpression pattern;
        Handler handler;
    };
    QList<Entry> m_handlers;
    QStringList m_invocations;
    QList<Options> m_options;   ///< parallel to m_invocations
    Options m_lastOptions;
};
