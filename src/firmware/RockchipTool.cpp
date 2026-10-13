// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "firmware/RockchipTool.h"

QString RockusbDevice::modeText() const
{
    switch (mode) {
    case Mode::Loader: return QStringLiteral("Loader");
    case Mode::Maskrom: return QStringLiteral("Maskrom");
    default: return QStringLiteral("Unknown");
    }
}
bool RockusbDevice::operator==(const RockusbDevice& o) const
{
    return devNo == o.devNo && vid == o.vid && pid == o.pid && locationId == o.locationId && mode == o.mode && serial == o.serial;
}
QString RkFirmwareInfo::summary() const { return QString(); }

RockchipTool::RockchipTool(ToolRunner* runner, QObject* parent) : QObject(parent), m_runner(runner) {}
void RockchipTool::setToolPath(const QString& path) { m_tool = path; }
QString RockchipTool::toolPath() const { return m_tool; }
void RockchipTool::setTimeoutMs(int ms) { m_timeoutMs = ms; }
int RockchipTool::timeoutMs() const { return m_timeoutMs; }
ToolRunner* RockchipTool::runner() const { return m_runner; }
ToolInvocation* RockchipTool::listDevices()
{
    ToolRunner::Options o; o.timeoutMs = m_timeoutMs;
    return m_runner->run(m_tool, {QStringLiteral("LD")}, o);
}
ToolInvocation* RockchipTool::firmwareInfo(const QString& image)
{
    ToolRunner::Options o; o.timeoutMs = m_timeoutMs;
    return m_runner->run(m_tool, {QStringLiteral("SFI"), image}, o);
}
ToolInvocation* RockchipTool::upgradeFirmware(const QString& image)
{
    return m_runner->run(m_tool, {QStringLiteral("UF"), image});
}
QList<RockusbDevice> RockchipTool::parseDeviceList(const QString&) { return {}; }
RkFirmwareInfo RockchipTool::parseFirmwareInfo(const QString&) { return {}; }
bool RockchipTool::reportsSuccess(const QString& output) { return output.contains(QLatin1String("Upgrade firmware ok"), Qt::CaseInsensitive); }
QString RockchipTool::stripBanner(const QString& output) { return output; }
QString RockchipTool::tailForMessage(const QString& output, int) { return output; }
bool RockchipTool::isRkfwImage(const QString&, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return false;
}
QList<RockchipTool::Progress> RockchipTool::ProgressParser::feed(const QByteArray&) { return {}; }
QList<RockchipTool::Progress> RockchipTool::ProgressParser::flush() { return {}; }
