#pragma once

#include <QByteArray>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "ssh/SshProfile.h"
#include "support/TestSshServer.h"

class FakeToolInvocation;
class FakeToolRunner;
class FakeUsbBus;

/**
 * What the firmware suites (tst_firmwaretab: the tab, tst_firmwarejob: the chain without widgets)
 * share: the facts of the Node board and its image as the real tools reported them on 2026-10-13,
 * the FakeToolRunner scripts of a flash and of the MCU bridge, and the in-process build machine.
 * Nothing here starts a real tool or touches a real board or the developer's ~/.ssh.
 */
namespace FirmwareFixtures {

bool writeFile(const QString& path, const QByteArray& bytes);
/// A file that starts with the Rockchip RKFW magic, like a real update.img.
QByteArray rkfwBytes(int size = 4096);

// ---- Tool outputs ----------------------------------------------------------------------------
extern const QByteArray kAdbBoard;      ///< `adb devices` with the board 0225ce3c8d8d6e44
extern const QByteArray kLdLoader;      ///< `upgrade_tool LD` with one board in Loader mode
extern const QByteArray kLdMaskrom;     ///< ... in MASKROM mode (written with UF, the whole image)
extern const QByteArray kProcVersion;   ///< `cat /proc/version; uptime` of the flashed board

// ---- The Node board's flash (facts of 2026-10-13) -------------------------------------------------
extern const QString kNodeMtdParts;
extern const QByteArray kNodeCmdline;
constexpr int kNodePartitionCount = 7;
extern const char* const kNodeNames[kNodePartitionCount];
extern const qint64 kNodeImageBytes[kNodePartitionCount];
extern const int kNodeSectors[kNodePartitionCount];
extern const int kNodeSizeSectors[kNodePartitionCount];   ///< partition sizes / 512: WL's count
QStringList nodeNames();

/// The Node image's env.img: a CRC, then "mtdparts=<layout>", "sys_bootargs=...", "sd_parts=..."
/// (NUL separated), padded to its 64 KiB.
QByteArray nodeEnvImage(const QString& layout);
/// A FakeToolRunner handler for `upgrade_tool EXF <image> <dir>`: writes what upgrade_tool v2.37
/// unpacks from the Node image into <dir>. An image whose file name contains "big" gets a boot.img
/// one byte larger than its partition; an image under a folder named "withenv..." gets a real
/// env.img carrying the Node layout (the real image's), the others an env.img without one.
void nodeExtractor(FakeToolInvocation* i);
/// The commands of a Loader-mode flash (the partition method) and of a MASKROM one (UF), SFI, the
/// ADB wait and the board info; `ld` is what `upgrade_tool LD` lists.
void scriptSocFlash(FakeToolRunner& runner, const QByteArray& ld);
/// The WL command lines a flash ran, in order.
QStringList wlCommands(const FakeToolRunner& runner);
/// The WL command line of Node partition `k` unpacked into `extractDir`.
QString expectedWl(int k, const QString& extractDir);
/// Where SocFlasher unpacks `image`: "<folder>/<base name>.extracted".
QString extractDirOf(const QString& image);
const QRegularExpression& exfPattern();

/// The board side of an MCU update as adb_link.py drives it: `adb devices`, the rfmcu probe,
/// `killall`, a forward that points at a FakeMcuServer (the stand-in for `rfmcu bridge`), the
/// bridge itself (prints "bridging", then runs until cancelled) and the forward removal.
void scriptMcuBridge(FakeToolRunner& runner, quint16 bridgePort);

/// (v0.7.0) The seven Node boards of the owner's bench (prompt_parallel_soc_flash.md), the port
/// keys as Windows prints them: A..G, serial, port.
struct FleetBench
{
    const char* tag;
    const char* serial;
    const char* port;
};
extern const FleetBench kFleetBench[7];
/// The seven bench boards on ADB in `bus`, every one Reboot::ToLoader, scripted into `runner`
/// (FakeUsbBus::script()) together with nodeExtractor for `upgrade_tool EXF`. A test that needs a
/// handler of its own registers it first (the first matching handler wins).
void scriptFleet(FakeUsbBus& bus, FakeToolRunner& runner);

/// The in-process build machine: TestSshServer serving `<root>/image/update.img`, a stored profile
/// with a test key (never the developer's ~/.ssh keys) and a known_hosts file holding the server.
/// Set the switches before start(): trustServer = false leaves known_hosts empty (the host key is
/// unknown); passwordAuth = true makes the server and the profile use `password` instead of a key.
struct BuildMachine
{
    QTemporaryDir root;
    std::unique_ptr<TestSshServer> server;
    SshProfile profile;
    QString imageFolder;
    QString knownHostsFile;
    QString keyFile;                   ///< the client key (empty with passwordAuth)
    QByteArray image = rkfwBytes(256 * 1024);
    bool trustServer = true;
    bool passwordAuth = false;
    QString password = QStringLiteral("secret");

    bool start(SshProfileStore* store, const QString& workDir);
    QString target() const;            ///< "test@127.0.0.1:<port>"
};

} // namespace FirmwareFixtures
