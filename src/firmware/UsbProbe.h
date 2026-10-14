#pragma once

#include <QList>
#include <QString>
#include <functional>

/**
 * What the operating system knows about attached Rockchip USB devices (VID 0x2207), independent
 * of adb and upgrade_tool, so the Firmware tab can say "a Rockchip device is plugged in but no
 * driver claims it: install the Rockchip USB drivers" (spec "Drivers").
 *
 * Windows: SetupAPI (SetupDiGetClassDevs DIGCF_PRESENT | DIGCF_ALLCLASSES) filtered on instance
 * ids starting with "USB\VID_2207"; `driverClass` is the device's setup class name
 * ("AndroidUsbDeviceClass" for the ADB driver, "Rockusb Device" / the class of rockusb.inf for the
 * loader, empty when no driver is bound - the Device Manager "unknown device" case), `problem`
 * the CM problem code (28 = CM_PROB_FAILED_INSTALL, 0 = fine). Linux: /sys/bus/usb/devices
 * (idVendor / idProduct / product), driverClass = the bound kernel driver when any. Other
 * platforms: an empty list.
 *
 * The USB port of a board (v0.7.0, the fleet flash; docs/DESIGN.md 4.13): the Node board's ADB
 * gadget carries no USB serial string, so Windows names its instance by its bus address -
 * `USB\VID_2207&PID_0006\<hub instance>&0&<port>` - and the same board in Loader mode is
 * `USB\VID_2207&PID_110E\<the same hub instance>&0&<the same port>` (measured 2026-10-14:
 * `6&38635FDB&0&2` before and after `adb reboot loader`). That suffix is the one stable name of a
 * board across ADB, Loader and MASKROM mode: portKey(). On Linux scanSysfs() synthesizes the same
 * shape from the sysfs device name ("USB\VID_2207&PID_0006\BUS&1-2.3"), which is a port path too.
 */
struct UsbDeviceInfo
{
    quint16 vid = 0;
    quint16 pid = 0;
    QString instanceId;    ///< "USB\VID_2207&PID_0006\0225CE3C8D8D6E44", or "USB\VID_2207&PID_0006\6&38635FDB&0&2" for a device without a USB serial string (the Node board)
    QString description;   ///< "Android ADB Interface", "Rockusb Device", "Unknown device"
    QString driverClass;   ///< empty = no driver bound
    unsigned problem = 0;  ///< Windows CM problem code, 0 = none
    /// (v0.7.0, log only) Windows: the first DEVPKEY_Device_LocationPaths entry
    /// ("PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(1)#USB(2)"; the key is a DEFINE_DEVPROPKEY, so
    /// UsbProbe.cpp includes <initguid.h> before <devpkey.h> - without it the GUID has no storage
    /// and su_core fails to link); Linux: the sysfs device name ("1-2.3"); "" elsewhere or when the
    /// property cannot be read. Nothing of the fleet depends on it.
    QString locationPath;
    bool hasDriver() const { return !driverClass.isEmpty() && problem == 0; }
    QString serial() const;   ///< the instance id's last segment when it is not a bus address (no '&')
    /// (v0.7.0) The port the device sits on, the same string for one board in ADB, Loader and
    /// MASKROM mode: the instance id's last segment when it is a bus address (it holds '&':
    /// "6&38635FDB&0&2" on Windows, "BUS&1-2.3" from scanSysfs() on Linux), kept exactly as the OS
    /// prints it (both instances of one board come from the same OS, so they compare equal
    /// verbatim); else locationPath (a device WITH a serial string: not the Node board's gadget);
    /// "" when neither is known. FleetFlasher's key for the port mapping.
    QString portKey() const;
    bool isAdbGadget() const { return vid == 0x2207 && pid == 0x0006; }   ///< the running firmware's ADB gadget
    /// A Rockchip device that is not the ADB gadget: BootROM / U-Boot rockusb (0x110e on the
    /// RV1106 in Loader mode; MASKROM is not told apart here, `upgrade_tool LD` says the mode) -
    /// per the brief, any VID 2207 pid but 0x0006 on a board's port is that board in download mode.
    bool isDownloadMode() const { return vid == 0x2207 && pid != 0x0006; }
};

namespace UsbProbe {

constexpr quint16 kRockchipVid = 0x2207;
constexpr quint16 kAdbPid = 0x0006;         ///< the running firmware's ADB gadget
constexpr quint16 kLoaderPidPrefix = 0x1100; ///< 0x110x: BootROM / U-Boot rockusb

/// Present devices with this vendor id (never throws; empty when the platform has no probe).
QList<UsbDeviceInfo> presentDevices(quint16 vid = kRockchipVid);

/// The sentence for the tab when adb / upgrade_tool see nothing but the bus does: names the
/// device and whether a driver is bound ("USB\VID_2207&PID_0006: no driver bound. Install the
/// Rockchip USB drivers (DriverAssistant) and replug the board."); empty when `devices` is empty.
QString driverHint(const QList<UsbDeviceInfo>& devices);

/// (v0.7.0) The enumeration behind presentDevices() as a value FleetFlasher takes
/// (setUsbEnumerator()): the application passes presentDevices, the tests FakeUsbBus::enumerate.
using Enumerator = std::function<QList<UsbDeviceInfo>(quint16 vid)>;

/// (v0.7.0) True where presentDevices() reports ports a board can be mapped by: Windows
/// (SetupAPI) and Linux (sysfs). Elsewhere a fleet run is refused with "port mapping not
/// available on this platform" before any board is touched (FleetFlasher::platformRefusal()).
constexpr bool hasPlatformProbe()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

/// (v0.7.0) One line for the log: "USB bus: 7 Rockchip devices (7 ADB, 0 in download mode)";
/// "USB bus: no Rockchip device" when `devices` is empty.
QString describe(const QList<UsbDeviceInfo>& devices);

} // namespace UsbProbe
