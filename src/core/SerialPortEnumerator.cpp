#include "core/SerialPortEnumerator.h"

#include <QSerialPortInfo>
#include <QStringList>
#include <QSet>
#include <QCoreApplication>
#include <algorithm>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/DeviceSimulator.h"

namespace {

constexpr int kDefaultPollIntervalMs = 1000;

QString hex4(quint16 value)
{
    return QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0')).toUpper();
}

SerialPortEntry entryFromInfo(const QSerialPortInfo& info)
{
    SerialPortEntry e;
    e.portName = info.portName();
    e.description = info.description();
    e.manufacturer = info.manufacturer();
    e.serialNumber = info.serialNumber();
    e.systemLocation = info.systemLocation();
    e.hasVidPid = info.hasVendorIdentifier() && info.hasProductIdentifier();
    e.vendorId = info.hasVendorIdentifier() ? info.vendorIdentifier() : 0;
    e.productId = info.hasProductIdentifier() ? info.productIdentifier() : 0;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------------------
// SerialPortEntry
// ---------------------------------------------------------------------------------------

QString SerialPortEntry::displayText() const
{
    if (description.trimmed().isEmpty()) {
        return portName;
    }
    return QStringLiteral("%1 - %2").arg(portName, description.trimmed());
}

QString SerialPortEntry::toolTip() const
{
    QStringList lines;
    lines.append(portName);
    if (!description.trimmed().isEmpty()) {
        lines.append(description.trimmed());
    }
    if (!manufacturer.trimmed().isEmpty()) {
        lines.append(QCoreApplication::translate("SerialPortEntry", "Manufacturer: %1").arg(manufacturer.trimmed()));
    }
    if (hasVidPid) {
        lines.append(QCoreApplication::translate("SerialPortEntry", "VID:PID: %1:%2")
                         .arg(hex4(vendorId), hex4(productId)));
    }
    if (!serialNumber.trimmed().isEmpty()) {
        lines.append(QCoreApplication::translate("SerialPortEntry", "Serial number: %1").arg(serialNumber.trimmed()));
    }
    if (!systemLocation.trimmed().isEmpty()) {
        lines.append(QCoreApplication::translate("SerialPortEntry", "Location: %1").arg(systemLocation.trimmed()));
    }
    const QString hint = kindHint();
    if (!hint.isEmpty()) {
        lines.append(hint);
    }
    return lines.join(QLatin1Char('\n'));
}

QString SerialPortEntry::kindHint() const
{
    if (hasVidPid) {
        switch (vendorId) {
        case 0x1A86:
            return QCoreApplication::translate("SerialPortEntry", "USB-UART bridge (WCH CH34x)");
        case 0x10C4:
            return QCoreApplication::translate("SerialPortEntry", "Silicon Labs CP210x");
        case 0x0403:
            return QCoreApplication::translate("SerialPortEntry", "FTDI");
        case 0x067B:
            return QCoreApplication::translate("SerialPortEntry", "Prolific PL2303");
        case 0x1366:
            return QCoreApplication::translate("SerialPortEntry", "SEGGER J-Link CDC");
        case 0x0483:
            return QCoreApplication::translate("SerialPortEntry", "STMicroelectronics");
        case 0x2207:
            return QCoreApplication::translate("SerialPortEntry", "Rockchip");
        case 0x303A:
            return QCoreApplication::translate("SerialPortEntry", "Espressif");
        case 0x2E8A:
            return QCoreApplication::translate("SerialPortEntry", "Raspberry Pi RP2040");
        default:
            break;
        }
    }
    if (description.contains(QLatin1String("Bluetooth"), Qt::CaseInsensitive)) {
        return QCoreApplication::translate("SerialPortEntry", "Bluetooth serial");
    }
    if (DeviceSimulator::isSimulatedPort(portName)) {
        return QCoreApplication::translate("SerialPortEntry", "Built-in device simulator (no hardware needed)");
    }
    return QString();
}

bool SerialPortEntry::operator==(const SerialPortEntry& other) const
{
    return portName == other.portName && description == other.description && manufacturer == other.manufacturer &&
           serialNumber == other.serialNumber && systemLocation == other.systemLocation &&
           vendorId == other.vendorId && productId == other.productId && hasVidPid == other.hasVidPid;
}

// ---------------------------------------------------------------------------------------
// SerialPortEnumerator
// ---------------------------------------------------------------------------------------

SerialPortEnumerator& SerialPortEnumerator::instance()
{
    static SerialPortEnumerator s_instance;
    return s_instance;
}

SerialPortEnumerator::SerialPortEnumerator(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(kDefaultPollIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SerialPortEnumerator::refresh);
}

QList<SerialPortEntry> SerialPortEnumerator::ports() const
{
    return m_ports;
}

QStringList SerialPortEnumerator::portNames() const
{
    QStringList names;
    names.reserve(m_ports.size());
    for (const SerialPortEntry& e : m_ports) {
        names.append(e.portName);
    }
    return names;
}

bool SerialPortEnumerator::contains(const QString& portName) const
{
    return std::any_of(m_ports.cbegin(), m_ports.cend(),
                       [&portName](const SerialPortEntry& e) { return e.portName == portName; });
}

int SerialPortEnumerator::pollInterval() const
{
    return m_timer.interval();
}

void SerialPortEnumerator::setPollInterval(int ms)
{
    m_timer.setInterval(qMax(100, ms));
}

bool SerialPortEnumerator::isRunning() const
{
    return m_timer.isActive();
}

void SerialPortEnumerator::start()
{
    if (m_timer.isActive()) {
        return;
    }
    qCInfo(lcSerial) << "port enumerator started, interval" << m_timer.interval() << "ms";
    refresh();
    m_timer.start();
}

void SerialPortEnumerator::stop()
{
    if (!m_timer.isActive()) {
        return;
    }
    m_timer.stop();
    qCInfo(lcSerial) << "port enumerator stopped";
}

bool SerialPortEnumerator::naturalLess(const QString& a, const QString& b)
{
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < a.size() && j < b.size()) {
        const QChar ca = a.at(i);
        const QChar cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            // Compare the digit runs numerically (skipping leading zeros, longer run wins).
            qsizetype startA = i;
            qsizetype startB = j;
            while (i < a.size() && a.at(i).isDigit()) {
                ++i;
            }
            while (j < b.size() && b.at(j).isDigit()) {
                ++j;
            }
            while (startA < i - 1 && a.at(startA) == QLatin1Char('0')) {
                ++startA;
            }
            while (startB < j - 1 && b.at(startB) == QLatin1Char('0')) {
                ++startB;
            }
            const qsizetype lenA = i - startA;
            const qsizetype lenB = j - startB;
            if (lenA != lenB) {
                return lenA < lenB;
            }
            const int cmp = QStringView(a).mid(startA, lenA).compare(QStringView(b).mid(startB, lenB));
            if (cmp != 0) {
                return cmp < 0;
            }
            continue;
        }
        const QChar la = ca.toLower();
        const QChar lb = cb.toLower();
        if (la != lb) {
            return la < lb;
        }
        ++i;
        ++j;
    }
    // One string is a prefix of the other (ignoring case): the shorter one sorts first.
    if ((a.size() - i) != (b.size() - j)) {
        return (a.size() - i) < (b.size() - j);
    }
    // Equal ignoring case: fall back to a case-sensitive compare for a strict weak ordering.
    return a.compare(b, Qt::CaseSensitive) < 0;
}

void SerialPortEnumerator::refresh()
{
    QList<SerialPortEntry> fresh;
    const QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
    fresh.reserve(infos.size());
    for (const QSerialPortInfo& info : infos) {
        fresh.append(entryFromInfo(info));
    }
    if (AppSettings::instance().showSimulatedPorts()) {
        // Built-in simulated devices; one that is "rebooting" is absent, like an unplugged board;
        // a powered-off one stays listed (reopening it powers it on).
        const QList<SerialPortEntry> simulated = DeviceSimulator::entries();
        for (const SerialPortEntry& entry : simulated) {
            if (DeviceSimulator::isListed(entry.portName)) {
                fresh.append(entry);
            }
        }
    }
    std::sort(fresh.begin(), fresh.end(), [](const SerialPortEntry& x, const SerialPortEntry& y) {
        return naturalLess(x.portName, y.portName);
    });

    if (fresh == m_ports) {
        return;
    }

    const QList<SerialPortEntry> old = m_ports;
    m_ports = fresh;

    QSet<QString> oldNames;
    for (const SerialPortEntry& e : old) {
        oldNames.insert(e.portName);
    }
    QSet<QString> newNames;
    for (const SerialPortEntry& e : fresh) {
        newNames.insert(e.portName);
    }

    for (const SerialPortEntry& e : old) {
        if (!newNames.contains(e.portName)) {
            qCInfo(lcSerial) << "port removed:" << e.portName;
            emit portRemoved(e.portName);
        }
    }
    for (const SerialPortEntry& e : fresh) {
        if (!oldNames.contains(e.portName)) {
            qCInfo(lcSerial) << "port added:" << e.portName << e.description;
            emit portAdded(e);
        }
    }
    emit portsChanged(m_ports);
}
