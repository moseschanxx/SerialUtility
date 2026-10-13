#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "firmware/ToolRunner.h"

/// One line of `adb devices`: serial and state ("device", "unauthorized", "offline", ...).
struct AdbDevice
{
    QString serial;
    QString state;
    bool operator==(const AdbDevice& other) const { return serial == other.serial && state == other.state; }
};

/**
 * adb as CC1310-Firmware/tools/adb_link.py and rockchip-platforms/tools/usb_update.sh use it.
 * Thin: every method starts one adb command through the ToolRunner and returns its
 * ToolInvocation (see ToolRunner.h for its lifetime); the static parsers turn the outputs into
 * values and are unit-tested on their own. The adb executable comes from setAdbPath()
 * (ToolLocator::locate(Tool::Adb) by the owner). Every command gets `-s <serial>` when a serial
 * is set, and a timeout (adbTimeoutMs, default 15 s like ADB_TIMEOUT) except the long-running
 * startBridge().
 */
class AdbTool : public QObject
{
    Q_OBJECT
public:
    explicit AdbTool(ToolRunner* runner, QObject* parent = nullptr);

    void setAdbPath(const QString& path);
    QString adbPath() const;
    void setSerial(const QString& serial);   ///< empty = no -s
    QString serial() const;
    void setTimeoutMs(int ms);               ///< default 15000
    int timeoutMs() const;
    ToolRunner* runner() const;

    // ---- Commands (asynchronous; the invocation reports) ---------------------------------
    ToolInvocation* devices();                                   ///< adb devices
    ToolInvocation* getState();                                  ///< adb get-state -> "device" / "unknown" / error text
    ToolInvocation* rebootLoader();                              ///< adb reboot loader
    ToolInvocation* shell(const QString& command);               ///< adb shell <command> (one argument)
    ToolInvocation* forwardAnyPort(quint16 remotePort);          ///< adb forward tcp:0 tcp:<remotePort> -> the local port
    ToolInvocation* removeForward(quint16 localPort);            ///< adb forward --remove tcp:<localPort>
    /// adb shell rfmcu bridge <port>: long-running (no timeout); cancel() ends it.
    ToolInvocation* startBridge(quint16 remotePort);
    ToolInvocation* run(const QStringList& arguments, int timeoutMs);   ///< any adb command

    // ---- Parsers (adb_link.py) -----------------------------------------------------------
    /// "List of devices attached\nc3d9\tdevice\n" -> [{c3d9, device}]; header and "* daemon" lines skipped.
    static QList<AdbDevice> parseDevices(const QString& output);
    /// adb_link.select_device(): the only ready device, or `wanted` when ready; otherwise empty
    /// with *error = "board X is 'unauthorized', not ready (unauthorized/offline: replug the
    /// cable)", "board X is not attached; attached: ...", "no ADB board attached...: connect the
    /// Node board's USB-C port to this PC (Windows needs the ADB driver from DriverAssistant once);
    /// the SoC firmware needs ADB and rfmcu: rockchip-platforms rv1106-bg2-main v1.2.0 or later",
    /// "several ADB boards attached (A, B): choose one".
    static QString selectDevice(const QList<AdbDevice>& devices, const QString& wanted, QString* error);
    /// The local port printed by `adb forward tcp:0 ...` ("41234\n" -> 41234); 0 when the output is
    /// not a number (an adb too old for tcp:0).
    static quint16 parseForwardedPort(const QString& output);
    /// adb get-state output -> "device" when the board is ready, else the trimmed text.
    static QString parseState(const QString& output);
    /// The firmware hint of adb_link.py (SOC_FIRMWARE_HINT).
    static QString socFirmwareHint();

private:
    ToolRunner* m_runner = nullptr;
    QString m_adb;
    QString m_serial;
    int m_timeoutMs = 15000;
};
