// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "firmware/UsbProbe.h"

QString UsbDeviceInfo::serial() const { return QString(); }

namespace UsbProbe {
QList<UsbDeviceInfo> presentDevices(quint16) { return {}; }
QString driverHint(const QList<UsbDeviceInfo>&) { return QString(); }
} // namespace UsbProbe
