#pragma once

#include <QList>
#include <QString>

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
 */
struct UsbDeviceInfo
{
    quint16 vid = 0;
    quint16 pid = 0;
    QString instanceId;    ///< "USB\VID_2207&PID_0006\0225CE3C8D8D6E44"
    QString description;   ///< "Android ADB Interface", "Rockusb Device", "Unknown device"
    QString driverClass;   ///< empty = no driver bound
    unsigned problem = 0;  ///< Windows CM problem code, 0 = none
    bool hasDriver() const { return !driverClass.isEmpty() && problem == 0; }
    QString serial() const;   ///< the instance id's last segment when it is not a bus address (no '&')
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

} // namespace UsbProbe
