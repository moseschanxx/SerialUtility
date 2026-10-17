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
/// A file that starts like a real update.img: the 0x66-byte RKFW header (magic, length 0x66,
/// version 0.0.00, built 2026-10-13 21:45:50, the chip code `chipTag` - its four ASCII bytes
/// stored reversed, as rkImageMaker writes them - and the offsets 0x66 / 0 / 0x66 / 0), then
/// padding. (v0.8.0) The default "110E" is the Node's; "1106" the stacked board's.
QByteArray rkfwBytes(int size = 4096, const QString& chipTag = QStringLiteral("110E"));

// ---- Tool outputs ----------------------------------------------------------------------------
extern const QByteArray kAdbBoard;      ///< `adb devices` with the board 0225ce3c8d8d6e44
extern const QByteArray kLdLoader;      ///< `upgrade_tool LD` with one board in Loader mode
extern const QByteArray kLdMaskrom;     ///< ... in MASKROM mode (written with UF, the whole image)
extern const QByteArray kProcVersion;   ///< `cat /proc/version; uptime` of the flashed board
// ---- (v0.8.0) the stacked board and the board profiles --------------------------------------
extern const QByteArray kLdLoaderStacked;   ///< kLdLoader with Pid=0x110c: the stacked board's U-Boot rockusb
extern const QByteArray kLdEmpty;           ///< the banner and "List of rockusb connected(0)": no device
extern const QByteArray kLdTwoLoaders;      ///< two devices in Loader mode: LocationIDs 104 (0x110e) and 112 (0x110c)
extern const QByteArray kProductStacked;    ///< the stacked gadget's product string as `cat` prints it
/// `adb [-s S] shell "cat /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product"` for addScript().
const QRegularExpression& productPattern();

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

// ---- (v0.8.0) The stacked board's flash (RV1106G2_FACTS.md rows 9, 10, 18) ----------------------
extern const QString kStackedMtdParts;
extern const QByteArray kStackedCmdline;
constexpr int kStackedPartitionCount = 6;
extern const char* const kStackedNames[kStackedPartitionCount];
extern const qint64 kStackedImageBytes[kStackedPartitionCount];   ///< boot 2 MiB: a stand-in for the 8-10 MiB build
extern const int kStackedSectors[kStackedPartitionCount];
extern const int kStackedSizeSectors[kStackedPartitionCount];
QStringList stackedNames();
/// The stacked image's env.img: "mtdparts=<layout>", "sys_bootargs= mtdparts=<layout>
/// rk_dma_heap_cma=48M" (no rootfstype, row 18), "sd_parts=...", padded to 64 KiB.
QByteArray stackedEnvImage(const QString& layout);
/// `upgrade_tool EXF` of the stacked image: six images, no rootfs; "big" in the image's file name
/// makes boot.img one byte larger than its 12 MiB partition, "withenv" in its path gives env.img
/// the stacked layout. scriptSocFlash() registers it for every image whose path holds "stacked".
void stackedExtractor(FakeToolInvocation* i);
/// The WL command line of stacked partition `k` unpacked into `extractDir`.
QString expectedStackedWl(int k, const QString& extractDir);
/// A FakeToolRunner handler for `upgrade_tool EXF <image> <dir>`: writes what upgrade_tool v2.37
/// unpacks from the Node image into <dir>. An image whose file name contains "big" gets a boot.img
/// one byte larger than its partition; an image under a folder named "withenv..." gets a real
/// env.img carrying the Node layout (the real image's), the others an env.img without one.
void nodeExtractor(FakeToolInvocation* i);
/// The commands of a Loader-mode flash (the partition method) and of a MASKROM one (UF), SFI, the
/// ADB wait and the board info; `ld` is what `upgrade_tool LD` lists. (v0.8.0) EXF of an image whose
/// path holds "stacked" runs stackedExtractor (registered first), every other one nodeExtractor.
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
