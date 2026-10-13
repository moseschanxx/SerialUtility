// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "support/FakeToolRunner.h"

FakeToolInvocation::FakeToolInvocation(const QString& program, const QStringList& arguments, QObject* parent)
    : ToolInvocation(parent), m_program(program), m_arguments(arguments)
{
    m_result.commandLine = ToolRunner::formatCommandLine(program, arguments);
}
void FakeToolInvocation::emitOutput(const QByteArray& chunk, int delayMs) { schedule(delayMs, [this, chunk] { appendOutput(chunk); }); }
void FakeToolInvocation::finish(int exitCode, int delayMs)
{
    schedule(delayMs, [this, exitCode] { ToolResult r = m_result; r.started = true; r.exitCode = exitCode; ToolInvocation::finish(r); });
}
void FakeToolInvocation::failToStart(const QString& message)
{
    schedule(0, [this, message] { ToolResult r = m_result; r.started = false; r.error = message; ToolInvocation::finish(r); });
}
void FakeToolInvocation::hang() { schedule(0, [this] { markStarted(); }); }
void FakeToolInvocation::writeInput(const QByteArray& data) { m_input.append(data); }
void FakeToolInvocation::closeInput() { m_inputClosed = true; }
void FakeToolInvocation::cancel()
{
    schedule(0, [this] { ToolResult r = m_result; r.started = true; r.cancelled = true; r.exitCode = -1; ToolInvocation::finish(r); });
}
void FakeToolInvocation::schedule(int delayMs, std::function<void()> step)
{
    m_pendingDelayMs += delayMs;
    QTimer::singleShot(m_pendingDelayMs, this, std::move(step));
}

FakeToolRunner::FakeToolRunner(QObject* parent) : ToolRunner(parent) {}
ToolInvocation* FakeToolRunner::run(const QString& program, const QStringList& arguments, const Options& options)
{
    auto* invocation = new FakeToolInvocation(program, arguments, this);
    m_invocations << invocation->commandLine();
    m_lastOptions = options;
    emit invocationStarted(invocation);
    for (const Entry& e : m_handlers) {
        if (e.pattern.match(invocation->commandLine()).hasMatch()) {
            e.handler(invocation);
            return invocation;
        }
    }
    invocation->emitOutput("fake: no handler for " + invocation->commandLine().toUtf8());
    invocation->finish(127);
    return invocation;
}
void FakeToolRunner::addScript(const QRegularExpression& commandLine, const QByteArray& output, int exitCode, int delayMs)
{
    addHandler(commandLine, [output, exitCode, delayMs](FakeToolInvocation* i) { i->emitOutput(output, delayMs); i->finish(exitCode); });
}
void FakeToolRunner::addHandler(const QRegularExpression& commandLine, Handler handler) { m_handlers.append({commandLine, std::move(handler)}); }
void FakeToolRunner::reset() { m_handlers.clear(); m_invocations.clear(); }
QList<FakeToolInvocation*> FakeToolRunner::liveInvocations() const
{
    QList<FakeToolInvocation*> live;
    for (auto* child : findChildren<FakeToolInvocation*>(Qt::FindDirectChildrenOnly)) {
        if (child->isRunning()) live << child;
    }
    return live;
}
int FakeToolRunner::count(const QRegularExpression& commandLine) const
{
    int n = 0;
    for (const QString& line : m_invocations) if (commandLine.match(line).hasMatch()) ++n;
    return n;
}
