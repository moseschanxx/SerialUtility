#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "firmware/UsbProbe.h"

class FakeToolRunner;

/**
 * The USB side of a fleet test (tst_fleetflasher, tst_firmwarecli): boards with an ADB serial and a
 * port key, each Absent, on ADB (Adb / Unauthorized), in Loader mode (with a LocationID the bus
 * assigns, counting from 100 in the order boards enter Loader mode, like upgrade_tool's per-location
 * numbers: a board entering Loader mode again on the same port gets the same number) or in Maskrom
 * mode. enumerate() is the UsbProbe::Enumerator for setUsbEnumerator(); ldOutput() /
 * adbDevicesOutput() are what the tools print for the current state (the shapes of 2026-10-14:
 * "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=112\tMode=Loader\tSerialNo="). script() registers the
 * FakeToolRunner handlers: `adb devices`; `adb -s S reboot loader` (after rebootDelayMs the ADB
 * instance is gone and LD lists the new LocationID; the enumeration shows the Loader instance
 * pnpLagMs later - the mapping must poll through that gap); `upgrade_tool LD` (finishes ldDurationMs
 * after its start, default 0, and answers from the bus state at THAT moment: a board that flips during
 * the LD is in its output); `upgrade_tool -s ID WL ...` (recorded per LocationID; an unknown id answers
 * "No found specific rockusb device!" with exit 255; writeDurationMs with "Write LBA from file (nn%)"
 * lines at 10 % steps; hangWrites(serial) holds the WL until releaseWrites()); `upgrade_tool -s ID RD`
 * (the board is back on ADB after resetDelayMs; "Reset Device OK."); `adb -s S get-state`; `adb -s S
 * shell "cat /proc/version"` / "cat /proc/cmdline" (the board's mtdParts, default kNodeMtdParts; ""
 * = a command line without mtdparts) / "cat /proc/version; uptime" (kProcVersion) / "rfmcu status"
 * ("pid=412 uart=owned mcu=alive\n"). A WL / RD / UF WITHOUT `-s` is answered "fake: unaddressed write
 * refused" with exit 99 so a test fails loudly, and counted. Misbehaviours of setReboot(): StaysOnAdb
 * (nothing changes), ToMaskrom (a 0x110c instance on the port, LD says Maskrom), DragsNext (the next
 * board of the bus moves to Loader at the same moment), GhostLoader (a Loader instance appears on the
 * port while the ADB instance stays), NoLdLine (the Loader instance appears but LD never lists it),
 * OtherPort (the Loader instance appears on `otherPort`), AdbKeepsListing (the Loader instance and the
 * LD line appear as ToLoader, but `adb devices` keeps listing the serial - adb's server holding a stale
 * entry), LdLineOnly (LD lists a new LocationID for the board but no instance ever enumerates and the
 * ADB instance stays: the LdOnly timeout), Vanishes (after rebootDelayMs the ADB instance is gone and
 * nothing appears, LD lists nothing: the LoaderNotEnumerated timeout and, with moveToLoader(), a late
 * arrival). leaveLoaderBeforeWrite(serial): the board is back on ADB once its mapping was confirmed
 * (right after the first `adb devices` that runs while it is in Loader mode), i.e. before any re-check
 * of the writing phase. reenumerate(serial) gives a Loader-mode board a new LocationID on the same port
 * (the enumeration unchanged, LD lists the new id, a WL / RD with the old id answers "No found specific
 * rockusb device!" exit 255); addForeignAdb(serial) adds an `adb devices` entry (state "device") with no
 * Rockchip instance on the bus - a phone. The counters tell what the flasher must never do:
 * ldDuringWrites() (an LD started while a WL / RD was in flight, OR a WL / RD started while an LD
 * was in flight), maxConcurrentWrites() (the wave size), unaddressedCommands(), writesTo(serial) /
 * resetsTo(serial) (the lines addressed to the board's id, in order), cancelledWrites() (WL / RD
 * invocations that got cancel(): must be 0).
 */
class FakeUsbBus : public QObject
{
    Q_OBJECT
public:
    enum class State { Absent, Adb, Unauthorized, Loader, Maskrom };
    enum class Reboot { ToLoader, StaysOnAdb, ToMaskrom, DragsNext, GhostLoader, NoLdLine, OtherPort, AdbKeepsListing, LdLineOnly, Vanishes };
    struct Board { QString serial; QString port; State state = State::Adb; QString locationId; Reboot reboot = Reboot::ToLoader; QString otherPort; QString mtdParts; bool ldListed = true; };

    explicit FakeUsbBus(QObject* parent = nullptr);
    ~FakeUsbBus() override;
    void addBoard(const QString& serial, const QString& port, State state = State::Adb);
    void setReboot(const QString& serial, Reboot behaviour, const QString& otherPort = QString());
    void setMtdParts(const QString& serial, const QString& mtdparts);
    void setRebootDelayMs(int ms);        ///< default 30
    void setPnpLagMs(int ms);             ///< default 10; 0 = the enumeration changes together with LD
    void setResetDelayMs(int ms);         ///< default 30
    void setLdDurationMs(int ms);         ///< default 0: how long `upgrade_tool LD` runs; it answers from the state at its END
    void setWriteDurationMs(int ms);      ///< default 40 per WL
    void setWriteFailure(const QString& serial, const QString& fileName, const QByteArray& output, int exitCode);
    void setResetFailure(const QString& serial, const QByteArray& output, int exitCode);
    void setNoAdbReturn(const QString& serial);           ///< after its RD the board stays Absent
    void leaveLoaderBeforeWrite(const QString& serial);   ///< back on ADB right before its first WL is asked for
    void leaveLoaderAfterRows(const QString& serial, int rows);   ///< back on ADB once `rows` WLs completed
    void hangWrites(const QString& serial);               ///< its WLs hang until releaseWrites()
    void releaseWrites();                                 ///< finish every hung WL (100 %, exit 0)
    void moveToAdb(const QString& serial);                ///< the test's own hand on the bus
    void moveToLoader(const QString& serial);             ///< ... a foreign Loader device, for instance
    void addForeignLoader(const QString& port);           ///< a Loader device that is no board of the bus
    void addForeignAdb(const QString& serial);            ///< an `adb devices` entry with no Rockchip instance on the bus (a phone)
    void reenumerate(const QString& serial);              ///< a new LocationID for a Loader-mode board, same port; the old id is gone

    QList<UsbDeviceInfo> enumerate(quint16 vid) const;    ///< UsbProbe::Enumerator
    QByteArray ldOutput() const;
    QByteArray adbDevicesOutput() const;
    void script(FakeToolRunner& runner);
    Board board(const QString& serial) const;
    QStringList writesTo(const QString& serial) const;
    QStringList resetsTo(const QString& serial) const;
    int maxConcurrentWrites() const;
    int ldDuringWrites() const;           ///< LD / WL-or-RD overlaps, either direction
    int unaddressedCommands() const;
    int cancelledWrites() const;

signals:
    void changed();   ///< a board changed state (the tests watch the bus)
private:
    struct Private; Private* d;
};
