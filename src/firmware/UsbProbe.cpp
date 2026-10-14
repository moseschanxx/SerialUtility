#include "firmware/UsbProbe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
// DEVPKEY_Device_LocationPaths is a DEFINE_DEVPROPKEY: only with INITGUID defined first does this
// translation unit give the key its storage (without it su_core fails to link on the key).
#include <initguid.h>
#include <devpkey.h>
#ifdef _MSC_VER
#pragma comment(lib, "cfgmgr32.lib")   // CM_Get_DevNode_Status; setupapi is linked by CMake
#endif
#endif

QString UsbDeviceInfo::serial() const
{
    // "USB\VID_2207&PID_0006\0225CE3C8D8D6E44": the last segment is the device's serial number;
    // a device without one gets a synthetic bus address ("6&1d2f&0&2") by Windows, which always
    // holds '&'.
    const QStringList parts = instanceId.split(QLatin1Char('\\'));
    if (parts.size() < 3) {
        return QString();
    }
    const QString last = parts.last();
    return last.contains(QLatin1Char('&')) ? QString() : last;
}

QString UsbDeviceInfo::portKey() const
{
    // A device without a USB serial string is named by its bus address, the same for the board's
    // ADB gadget and its Loader / MASKROM device ("6&38635FDB&0&2"): kept exactly as printed.
    const QStringList parts = instanceId.split(QLatin1Char('\\'));
    if (parts.size() >= 3 && parts.last().contains(QLatin1Char('&'))) {
        return parts.last();
    }
    return locationPath;
}

namespace UsbProbe {

namespace {

#ifdef Q_OS_WIN

/// A REG_SZ device property (SPDRP_*), empty when the device has none: a device without a driver
/// has no class, and the Device Manager shows it as "Unknown device".
QString stringProperty(HDEVINFO set, SP_DEVINFO_DATA* device, DWORD property)
{
    wchar_t buffer[512] = {};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, device, property, &type, reinterpret_cast<PBYTE>(buffer),
                                           static_cast<DWORD>(sizeof(buffer) - sizeof(wchar_t)), nullptr)
        || type != REG_SZ) {
        return QString();
    }
    return QString::fromWCharArray(buffer).trimmed();
}

/// The first entry of DEVPKEY_Device_LocationPaths ("PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(1)#USB(2)"),
/// a string list; "" when the device has none. Log only: nothing depends on it.
QString locationPathProperty(HDEVINFO set, SP_DEVINFO_DATA* device)
{
    wchar_t buffer[1024] = {};
    DEVPROPTYPE type = 0;
    if (!SetupDiGetDevicePropertyW(set, device, &DEVPKEY_Device_LocationPaths, &type, reinterpret_cast<PBYTE>(buffer),
                                   static_cast<DWORD>(sizeof(buffer) - 2 * sizeof(wchar_t)), nullptr, 0)
        || (type != DEVPROP_TYPE_STRING_LIST && type != DEVPROP_TYPE_STRING)) {
        return QString();
    }
    return QString::fromWCharArray(buffer).trimmed();   // a REG_MULTI_SZ: the first string ends at its NUL
}

QList<UsbDeviceInfo> enumerateWindows(quint16 vid)
{
    QList<UsbDeviceInfo> found;
    const HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) {
        return found;
    }

    static const QRegularExpression idPattern(QStringLiteral("^USB\\\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})"),
                                              QRegularExpression::CaseInsensitiveOption);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &device, id, MAX_DEVICE_ID_LEN, nullptr)) {
            continue;
        }
        const QString instanceId = QString::fromWCharArray(id);
        const QRegularExpressionMatch match = idPattern.match(instanceId);
        if (!match.hasMatch() || match.captured(1).toUShort(nullptr, 16) != vid) {
            continue;
        }

        UsbDeviceInfo info;
        info.vid = vid;
        info.pid = match.captured(2).toUShort(nullptr, 16);
        info.instanceId = instanceId;
        // The friendly name is what the Device Manager shows; the INF's description is the fallback.
        info.description = stringProperty(set, &device, SPDRP_FRIENDLYNAME);
        if (info.description.isEmpty()) {
            info.description = stringProperty(set, &device, SPDRP_DEVICEDESC);
        }
        if (info.description.isEmpty()) {
            info.description = QCoreApplication::translate("UsbProbe", "Unknown device");
        }
        info.driverClass = stringProperty(set, &device, SPDRP_CLASS);
        info.locationPath = locationPathProperty(set, &device);
        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) == CR_SUCCESS) {
            info.problem = static_cast<unsigned>(problem);
        }
        found.append(info);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

#endif // Q_OS_WIN

QString readSysfsText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.read(256)).trimmed();
}

} // namespace

/// Linux: one directory per USB device under /sys/bus/usb/devices ("1-2"; the "1-2:1.0" interface
/// directories have no idVendor and are skipped). Public inside this library (not in the header)
/// so the tests can run it on a fake tree. `driverClass` is the kernel driver bound to the device
/// itself ("usb" for every enumerated device: libusb needs no other, so Linux never reports a
/// missing driver - a permissions problem looks different and is udev's business).
QList<UsbDeviceInfo> scanSysfs(const QString& sysfsRoot, quint16 vid)
{
    QList<UsbDeviceInfo> found;
    const QDir root(sysfsRoot);
    for (const QString& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString base = root.filePath(name);
        bool ok = false;
        const quint16 deviceVid = static_cast<quint16>(readSysfsText(base + QStringLiteral("/idVendor")).toUInt(&ok, 16));
        if (!ok || deviceVid != vid) {
            continue;
        }
        UsbDeviceInfo info;
        info.vid = deviceVid;
        info.pid = static_cast<quint16>(readSysfsText(base + QStringLiteral("/idProduct")).toUInt(nullptr, 16));
        const QString serial = readSysfsText(base + QStringLiteral("/serial"));
        // Same shape as the Windows instance id, so serial() and the hint read alike; a device
        // without a serial gets a bus address, which holds '&' like Windows' synthetic ones.
        info.instanceId = QStringLiteral("USB\\VID_%1&PID_%2\\%3")
                              .arg(info.vid, 4, 16, QLatin1Char('0'))
                              .arg(info.pid, 4, 16, QLatin1Char('0'))
                              .arg(serial.isEmpty() ? QStringLiteral("bus&") + name : serial)
                              .toUpper();
        info.locationPath = name;   // "1-2.3": the port path, like Windows' LocationPaths
        info.description = readSysfsText(base + QStringLiteral("/product"));
        if (info.description.isEmpty()) {
            info.description = QCoreApplication::translate("UsbProbe", "Unknown device");
        }
        const QFileInfo driver(base + QStringLiteral("/driver"));
        if (driver.isSymLink()) {
            info.driverClass = QFileInfo(driver.symLinkTarget()).fileName();
        }
        found.append(info);
    }
    return found;
}

QList<UsbDeviceInfo> presentDevices(quint16 vid)
{
    QList<UsbDeviceInfo> devices;
#if defined(Q_OS_WIN)
    devices = enumerateWindows(vid);
#elif defined(Q_OS_LINUX)
    devices = scanSysfs(QStringLiteral("/sys/bus/usb/devices"), vid);
#else
    Q_UNUSED(vid)
#endif
    std::sort(devices.begin(), devices.end(),
              [](const UsbDeviceInfo& a, const UsbDeviceInfo& b) { return a.instanceId < b.instanceId; });
    return devices;
}

QString driverHint(const QList<UsbDeviceInfo>& devices)
{
    if (devices.isEmpty()) {
        return QString();
    }
    QStringList sentences;
    for (const UsbDeviceInfo& device : devices) {
        if (device.driverClass.isEmpty()) {
            sentences << QCoreApplication::translate("UsbProbe", "%1: no driver bound. Install the Rockchip USB drivers (DriverAssistant) and replug the board.")
                             .arg(device.instanceId);
        } else if (device.problem != 0) {
            sentences << QCoreApplication::translate("UsbProbe", "%1: the driver reports problem code %2. Reinstall the Rockchip USB drivers "
                            "(DriverAssistant) and replug the board.")
                             .arg(device.instanceId)
                             .arg(device.problem);
        }
    }
    if (sentences.isEmpty()) {
        // Every device has its driver, yet neither adb nor upgrade_tool lists the board.
        const UsbDeviceInfo& device = devices.first();
        sentences << QCoreApplication::translate("UsbProbe", "%1: driver %2 is bound, but neither adb nor upgrade_tool lists the board. Replug the "
                        "cable; if it stays hidden, reinstall the Rockchip USB drivers (DriverAssistant).")
                         .arg(device.instanceId, device.driverClass);
    }
    sentences.removeDuplicates();
    return sentences.join(QLatin1Char(' '));
}

QString describe(const QList<UsbDeviceInfo>& devices)
{
    if (devices.isEmpty()) {
        return QCoreApplication::translate("UsbProbe", "USB bus: no Rockchip device");
    }
    int adb = 0;
    int download = 0;
    for (const UsbDeviceInfo& device : devices) {
        adb += device.isAdbGadget() ? 1 : 0;
        download += device.isDownloadMode() ? 1 : 0;
    }
    return devices.size() == 1
               ? QCoreApplication::translate("UsbProbe", "USB bus: 1 Rockchip device (%1 ADB, %2 in download mode)").arg(adb).arg(download)
               : QCoreApplication::translate("UsbProbe", "USB bus: %1 Rockchip devices (%2 ADB, %3 in download mode)")
                     .arg(devices.size())
                     .arg(adb)
                     .arg(download);
}

} // namespace UsbProbe
