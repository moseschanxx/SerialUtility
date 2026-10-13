#include "support/FakeToolRunner.h"

#include <QPointer>
#include <QQueue>

#include <utility>

namespace {

/**
 * The delivery queue of one FakeToolInvocation, a child object of it: steps run strictly one
 * after the other, each after its own delay counted from the end of the previous one (so a
 * step queued late is not held back by delays that have long passed), and always from the
 * event loop. cancel() empties it.
 */
class FakeStepQueue : public QObject
{
public:
    explicit FakeStepQueue(QObject* parent) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this] { runNext(); });
    }

    /// Returns the number of steps pending after the call (the invocation keeps it in m_pendingDelayMs's place).
    int enqueue(int delayMs, std::function<void()> action)
    {
        m_steps.enqueue({qMax(0, delayMs), std::move(action)});
        if (!m_timer.isActive()) {
            m_timer.start(m_steps.head().delayMs);
        }
        return static_cast<int>(m_steps.size());
    }

    void clear()
    {
        m_steps.clear();
        m_timer.stop();
    }

private:
    struct Step
    {
        int delayMs = 0;
        std::function<void()> action;
    };

    void runNext()
    {
        if (m_steps.isEmpty()) {
            return;
        }
        Step step = m_steps.dequeue();
        const QPointer<FakeStepQueue> self(this);   // a slot may delete the invocation (and with it this queue)
        step.action();   // may enqueue more, clear() or finish the invocation
        if (!self) {
            return;
        }
        if (!m_steps.isEmpty() && !m_timer.isActive()) {
            m_timer.start(m_steps.head().delayMs);
        }
    }

    QQueue<Step> m_steps;
    QTimer m_timer;
};

/// The queue is a plain QObject child (no Q_OBJECT: this file is not moc'ed), so it is found with
/// dynamic_cast instead of QObject::findChild().
FakeStepQueue* queueOf(QObject* invocation)
{
    for (QObject* child : invocation->children()) {
        if (auto* queue = dynamic_cast<FakeStepQueue*>(child)) {
            return queue;
        }
    }
    return new FakeStepQueue(invocation);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// FakeToolInvocation
// ---------------------------------------------------------------------------------------------

FakeToolInvocation::FakeToolInvocation(const QString& program, const QStringList& arguments, QObject* parent)
    : ToolInvocation(parent), m_program(program), m_arguments(arguments)
{
    m_result.commandLine = ToolRunner::formatCommandLine(program, arguments);
    m_running = true;   // "running" from creation to finish(), like a process that is under way
}

void FakeToolInvocation::schedule(int delayMs, std::function<void()> action)
{
    // m_pendingDelayMs counts the deliveries still queued (the queue itself lives in a child object).
    m_pendingDelayMs = queueOf(this)->enqueue(delayMs, [this, run = std::move(action)] {
        --m_pendingDelayMs;
        run();
    });
}

void FakeToolInvocation::emitOutput(const QByteArray& chunk, int delayMs)
{
    schedule(delayMs, [this, chunk] {
        markStarted();   // started() always precedes the first output
        appendOutput(chunk);
    });
}

void FakeToolInvocation::finish(int exitCode, int delayMs)
{
    schedule(delayMs, [this, exitCode] {
        markStarted();
        ToolResult result = m_result;
        result.started = true;
        result.exitCode = exitCode;
        ToolInvocation::finish(result);
    });
}

void FakeToolInvocation::failToStart(const QString& message)
{
    schedule(0, [this, message] {
        ToolResult result = m_result;
        result.started = false;
        result.exitCode = -1;
        result.error = message;
        ToolInvocation::finish(result);
    });
}

void FakeToolInvocation::hang()
{
    schedule(0, [this] { markStarted(); });
}

void FakeToolInvocation::writeInput(const QByteArray& data)
{
    if (!m_finished) {
        m_input.append(data);
    }
}

void FakeToolInvocation::closeInput()
{
    if (!m_finished) {
        m_inputClosed = true;
    }
}

void FakeToolInvocation::cancel()
{
    if (m_finished) {
        return;   // a no-op on a finished command, as the contract says
    }
    // Whatever output or exit was still queued never happens: the cancel wins.
    queueOf(this)->clear();
    m_pendingDelayMs = 0;
    schedule(0, [this] {
        markStarted();
        ToolResult result = m_result;
        result.started = true;
        result.cancelled = true;
        result.exitCode = -1;
        ToolInvocation::finish(result);
    });
}

// ---------------------------------------------------------------------------------------------
// FakeToolRunner
// ---------------------------------------------------------------------------------------------

FakeToolRunner::FakeToolRunner(QObject* parent) : ToolRunner(parent) {}

ToolInvocation* FakeToolRunner::run(const QString& program, const QStringList& arguments, const Options& options)
{
    auto* invocation = new FakeToolInvocation(program, arguments, this);
    const QString commandLine = invocation->commandLine();
    m_invocations << commandLine;
    m_options << options;
    m_lastOptions = options;
    emit invocationStarted(invocation);

    // Copy the handler out: it may call reset() or addHandler() on this runner.
    Handler handler;
    for (const Entry& entry : std::as_const(m_handlers)) {
        if (entry.pattern.match(commandLine).hasMatch()) {
            handler = entry.handler;
            break;
        }
    }
    if (handler) {
        handler(invocation);
    } else {
        invocation->emitOutput("fake: no handler for " + commandLine.toUtf8());
        invocation->finish(127);
    }
    return invocation;
}

void FakeToolRunner::addScript(const QRegularExpression& commandLine, const QByteArray& output, int exitCode, int delayMs)
{
    addHandler(commandLine, [output, exitCode, delayMs](FakeToolInvocation* invocation) {
        invocation->emitOutput(output, delayMs);
        invocation->finish(exitCode);
    });
}

void FakeToolRunner::addHandler(const QRegularExpression& commandLine, Handler handler)
{
    m_handlers.append({commandLine, std::move(handler)});
}

void FakeToolRunner::reset()
{
    m_handlers.clear();
    m_invocations.clear();
    m_options.clear();
}

QList<FakeToolInvocation*> FakeToolRunner::liveInvocations() const
{
    QList<FakeToolInvocation*> live;
    for (auto* child : findChildren<FakeToolInvocation*>(Qt::FindDirectChildrenOnly)) {
        if (child->isRunning()) {
            live << child;
        }
    }
    return live;
}

int FakeToolRunner::count(const QRegularExpression& commandLine) const
{
    int matching = 0;
    for (const QString& line : m_invocations) {
        if (commandLine.match(line).hasMatch()) {
            ++matching;
        }
    }
    return matching;
}
