#include "firmware/AdbTool.h"

#include <QRegularExpression>

namespace {

/// Lines of tool output, split at "\r\n", "\r" and "\n" (adb on Windows ends lines with "\r\n").
QStringList outputLines(const QString& output)
{
    QString text = output;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.split(QLatin1Char('\n'));
}

} // namespace

AdbTool::AdbTool(ToolRunner* runner, QObject* parent) : QObject(parent), m_runner(runner) {}
void AdbTool::setAdbPath(const QString& path) { m_adb = path; }
QString AdbTool::adbPath() const { return m_adb; }
void AdbTool::setSerial(const QString& serial) { m_serial = serial; }
QString AdbTool::serial() const { return m_serial; }
void AdbTool::setTimeoutMs(int ms) { m_timeoutMs = ms; }
int AdbTool::timeoutMs() const { return m_timeoutMs; }
ToolRunner* AdbTool::runner() const { return m_runner; }

// ---- Commands -------------------------------------------------------------------------------

ToolInvocation* AdbTool::devices()
{
    return run({QStringLiteral("devices")}, m_timeoutMs);
}

ToolInvocation* AdbTool::getState()
{
    return run({QStringLiteral("get-state")}, m_timeoutMs);
}

ToolInvocation* AdbTool::rebootLoader()
{
    return run({QStringLiteral("reboot"), QStringLiteral("loader")}, m_timeoutMs);
}

ToolInvocation* AdbTool::shell(const QString& command)
{
    // One argument, like adb_link.py: adb joins it for the board's shell, so pipes, `;` and `||`
    // in `command` run on the board, not on this PC.
    return run({QStringLiteral("shell"), command}, m_timeoutMs);
}

ToolInvocation* AdbTool::forwardAnyPort(quint16 remotePort)
{
    return run({QStringLiteral("forward"), QStringLiteral("tcp:0"), QStringLiteral("tcp:%1").arg(remotePort)},
               m_timeoutMs);
}

ToolInvocation* AdbTool::removeForward(quint16 localPort)
{
    return run({QStringLiteral("forward"), QStringLiteral("--remove"), QStringLiteral("tcp:%1").arg(localPort)},
               m_timeoutMs);
}

ToolInvocation* AdbTool::startBridge(quint16 remotePort)
{
    // Long-running: it ends only when the bridge does (or when the owner cancels it), so no timeout.
    return run({QStringLiteral("shell"), QStringLiteral("rfmcu"), QStringLiteral("bridge"),
                QString::number(remotePort)},
               0);
}

ToolInvocation* AdbTool::run(const QStringList& arguments, int timeoutMs)
{
    QStringList args;
    if (!m_serial.isEmpty()) {
        args << QStringLiteral("-s") << m_serial;
    }
    args << arguments;
    ToolRunner::Options options;
    options.timeoutMs = timeoutMs;
    return m_runner->run(m_adb, args, options);
}

// ---- Parsers --------------------------------------------------------------------------------

QList<AdbDevice> AdbTool::parseDevices(const QString& output)
{
    QList<AdbDevice> devices;
    for (const QString& rawLine : outputLines(output)) {
        const QString line = rawLine.trimmed();
        // adb_link.parse_devices(): the header and "* daemon ..." lines carry no device. The
        // "adb server version (n) doesn't match this client" notice and "error:" lines are not
        // devices either (the Python version would read them as serial "adb", state "server").
        if (line.isEmpty() || line.startsWith(QLatin1String("List of devices"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1Char('*')) || line.startsWith(QLatin1String("adb "), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("adb:"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("error:"), Qt::CaseInsensitive)) {
            continue;
        }
        const QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.size() >= 2) {
            devices.append({fields.at(0), fields.at(1)});
        }
    }
    return devices;
}

QString AdbTool::selectDevice(const QList<AdbDevice>& devices, const QString& wanted, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return QString();
    };
    if (error) {
        error->clear();
    }

    if (!wanted.isEmpty()) {
        for (const AdbDevice& device : devices) {
            if (device.serial == wanted) {
                if (device.state != QLatin1String("device")) {
                    return fail(tr("board %1 is '%2', not ready (unauthorized/offline: replug the cable)")
                                    .arg(device.serial, device.state));
                }
                return device.serial;
            }
        }
        QStringList attached;
        for (const AdbDevice& device : devices) {
            attached << device.serial;
        }
        return fail(tr("board %1 is not attached; attached: %2")
                        .arg(wanted, attached.isEmpty() ? tr("none") : attached.join(QStringLiteral(", "))));
    }

    QStringList ready;
    for (const AdbDevice& device : devices) {
        if (device.state == QLatin1String("device")) {
            ready << device.serial;
        }
    }
    if (ready.size() == 1) {
        return ready.first();
    }
    if (ready.isEmpty()) {
        QStringList others;
        for (const AdbDevice& device : devices) {
            others << QStringLiteral("%1 (%2)").arg(device.serial, device.state);
        }
        const QString found = others.isEmpty() ? QString()
                                               : tr(" in a ready state, found %1").arg(others.join(QStringLiteral(", ")));
        return fail(tr("no ADB board attached%1: connect the Node board's USB-C port to this PC "
                       "(Windows needs the ADB driver from DriverAssistant once); %2")
                        .arg(found, socFirmwareHint()));
    }
    return fail(tr("several ADB boards attached (%1): choose one").arg(ready.join(QStringLiteral(", "))));
}

quint16 AdbTool::parseForwardedPort(const QString& output)
{
    // `adb forward tcp:0 tcp:7000` prints the allocated port on a line of its own. A daemon start
    // notice may come first, so the last non-empty line decides; anything but a port number (an
    // adb too old for tcp:0 prints nothing or an error) is 0.
    const QStringList lines = outputLines(output);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (line.isEmpty()) {
            continue;
        }
        bool ok = false;
        const uint port = line.toUInt(&ok, 10);
        // QString::toUInt() accepts "+5" and " 5": insist on plain digits like str.isdigit().
        const bool plainDigits = ok && line.at(0).isDigit() && line.at(line.size() - 1).isDigit();
        return plainDigits && port >= 1 && port <= 65535 ? static_cast<quint16>(port) : 0;
    }
    return 0;
}

QString AdbTool::parseState(const QString& output)
{
    for (const QString& line : outputLines(output)) {
        if (line.trimmed() == QLatin1String("device")) {
            return QStringLiteral("device");   // daemon notices may precede the state
        }
    }
    return output.trimmed();
}

QString AdbTool::socFirmwareHint()
{
    // adb_link.SOC_FIRMWARE_HINT without its pointer into the CC1310 repo's runbook: the SoC
    // update is a button of the Firmware tab.
    return tr("the SoC firmware needs ADB and rfmcu: rockchip-platforms rv1106-bg2-main v1.2.0 or later");
}
