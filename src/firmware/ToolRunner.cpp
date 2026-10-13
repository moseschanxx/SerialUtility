// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "firmware/ToolRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QTimer>

QString ToolResult::outputText() const
{
    QString text = QString::fromUtf8(output);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

ToolInvocation::ToolInvocation(QObject* parent) : QObject(parent) {}
ToolInvocation::~ToolInvocation() = default;
void ToolInvocation::markStarted() { m_running = true; m_result.started = true; emit started(); }
void ToolInvocation::appendOutput(const QByteArray& chunk) { m_result.output.append(chunk); emit outputReceived(chunk); }
void ToolInvocation::finish(const ToolResult& result)
{
    if (m_finished) return;
    m_result = result;
    m_running = false;
    m_finished = true;
    emit finished(m_result);
    if (m_autoDelete) deleteLater();
}

ToolRunner::ToolRunner(QObject* parent) : QObject(parent) {}
ToolRunner::~ToolRunner() = default;
QString ToolRunner::formatCommandLine(const QString& program, const QStringList& arguments)
{
    QStringList parts{program};
    for (const QString& a : arguments) parts << (a.contains(QLatin1Char(' ')) ? QLatin1Char('"') + a + QLatin1Char('"') : a);
    return parts.join(QLatin1Char(' '));
}

namespace {
class StubInvocation : public ToolInvocation
{
public:
    explicit StubInvocation(const QString& commandLine, QObject* parent) : ToolInvocation(parent)
    {
        m_result.commandLine = commandLine;
        QTimer::singleShot(0, this, [this] {
            ToolResult r = m_result;
            r.error = QStringLiteral("ProcessToolRunner not implemented");
            finish(r);
        });
    }
    void writeInput(const QByteArray&) override {}
    void closeInput() override {}
    void cancel() override {}
};
} // namespace

ProcessToolRunner::ProcessToolRunner(QObject* parent) : ToolRunner(parent) {}
ToolInvocation* ProcessToolRunner::run(const QString& program, const QStringList& arguments, const Options&)
{
    return new StubInvocation(formatCommandLine(program, arguments), this);
}

namespace ToolLocator {
QString bundledToolsDirectory() { return QCoreApplication::applicationDirPath() + QStringLiteral("/tools"); }
QString fileName(Tool tool)
{
    switch (tool) {
    case Tool::UpgradeTool: return QStringLiteral("upgrade_tool");
    case Tool::Adb: return QStringLiteral("adb");
    case Tool::DriverInstaller: return QStringLiteral("DriverAssistant/DriverInstall.exe");
    }
    return QString();
}
QString bundledPath(Tool tool) { return bundledToolsDirectory() + QLatin1Char('/') + fileName(tool); }
QString locate(Tool, const QString& override) { return override; }
QString describe(Tool, const QString&) { return QStringLiteral("not found"); }
} // namespace ToolLocator
