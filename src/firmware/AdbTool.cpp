// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "firmware/AdbTool.h"

AdbTool::AdbTool(ToolRunner* runner, QObject* parent) : QObject(parent), m_runner(runner) {}
void AdbTool::setAdbPath(const QString& path) { m_adb = path; }
QString AdbTool::adbPath() const { return m_adb; }
void AdbTool::setSerial(const QString& serial) { m_serial = serial; }
QString AdbTool::serial() const { return m_serial; }
void AdbTool::setTimeoutMs(int ms) { m_timeoutMs = ms; }
int AdbTool::timeoutMs() const { return m_timeoutMs; }
ToolRunner* AdbTool::runner() const { return m_runner; }
ToolInvocation* AdbTool::devices() { return run({QStringLiteral("devices")}, m_timeoutMs); }
ToolInvocation* AdbTool::getState() { return run({QStringLiteral("get-state")}, m_timeoutMs); }
ToolInvocation* AdbTool::rebootLoader() { return run({QStringLiteral("reboot"), QStringLiteral("loader")}, m_timeoutMs); }
ToolInvocation* AdbTool::shell(const QString& command) { return run({QStringLiteral("shell"), command}, m_timeoutMs); }
ToolInvocation* AdbTool::forwardAnyPort(quint16 remotePort)
{
    return run({QStringLiteral("forward"), QStringLiteral("tcp:0"), QStringLiteral("tcp:%1").arg(remotePort)}, m_timeoutMs);
}
ToolInvocation* AdbTool::removeForward(quint16 localPort)
{
    return run({QStringLiteral("forward"), QStringLiteral("--remove"), QStringLiteral("tcp:%1").arg(localPort)}, m_timeoutMs);
}
ToolInvocation* AdbTool::startBridge(quint16 remotePort)
{
    return run({QStringLiteral("shell"), QStringLiteral("rfmcu"), QStringLiteral("bridge"), QString::number(remotePort)}, 0);
}
ToolInvocation* AdbTool::run(const QStringList& arguments, int timeoutMs)
{
    QStringList args;
    if (!m_serial.isEmpty()) args << QStringLiteral("-s") << m_serial;
    args << arguments;
    ToolRunner::Options options;
    options.timeoutMs = timeoutMs;
    return m_runner->run(m_adb, args, options);
}
QList<AdbDevice> AdbTool::parseDevices(const QString&) { return {}; }
QString AdbTool::selectDevice(const QList<AdbDevice>&, const QString&, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return QString();
}
quint16 AdbTool::parseForwardedPort(const QString&) { return 0; }
QString AdbTool::parseState(const QString& output) { return output.trimmed(); }
QString AdbTool::socFirmwareHint() { return QString(); }
