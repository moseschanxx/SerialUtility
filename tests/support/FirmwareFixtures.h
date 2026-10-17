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
// ---- (v0.8.0, layout-pl) The layout read of a board met in Loader mode ------------------------------
/// `upgrade_tool -s <id> PL` as the tool's documented table (the shape on a NOR board whose table
/// lives in env.img is UNVERIFIED, open question 7): the banner, "List of partitions(7)", the header
/// "No  Offset(LBA)  Size(LBA)  Name", then the Node's seven partitions in sectors ("01  0x00000000
/// 0x00000080  env" ... "07  0x00004780  0x00001800  userdata"), "\r\n" line ends.
extern const QByteArray kPlNode;
extern const QByteArray kPlStacked;      ///< the stacked board's six: boot 0x780 / 0x6000, userdata 0x6780 / 0x1800
extern const QByteArray kPlDecimal;      ///< the Node's table with decimal numbers in "name offset size" order
extern const QByteArray kPlGarbage;      ///< the banner and "List of partitions(0)" only
extern const QByteArray kPlHeaderOnly;   ///< the banner, "List of partitions(7)" and the header line, no row
/// Handlers for the Loader-mode layout read, registered BEFORE scriptSocFlash() (the first match
/// wins; scriptSocFlash() itself has no PL / RL: the pinned command lists count on their absence):
/// `^upgrade_tool -s \d+ RL 0 128 ` writes `envImage` to the command's file argument (its folder
/// created) and answers "Read LBA from file (100%)" with exit 0 - an empty `envImage` answers
/// "Read LBA failed!" with exit 1 -; `^upgrade_tool -s \d+ PL$` answers `plOutput` with `plExitCode`
/// (an empty `plOutput` with exit 0: the tool printed nothing).
void scriptLoaderLayoutRead(FakeToolRunner& runner, const QByteArray& envImage, const QByteArray& plOutput, int plExitCode);
/// The two patterns above, for tests that register their own handlers.
const QRegularExpression& loaderEnvReadPattern();
const QRegularExpression& loaderPartitionListPattern();

// ---- (v0.8.0, verify-rl) Verify after write: a flash that remembers what was written ------------
/// (v0.8.0, verify-rl) A flash model for the single-board suites: every `upgrade_tool WL <sector>
/// <count> <file>` (and `-s <id> WL ...`) is recorded as sector -> file and answered as scriptSocFlash()'s
/// WL script does; every `upgrade_tool -s <id> RL <sector> <count> <file>` writes the bytes of the file
/// written at that sector (padded with 0xff to count * 512; nothing written there -> all 0xff) to
/// the command's file, with "Read LBA from file (10%)" ... "(100%)" lines, exit 0 - unless told
/// otherwise: corruptByte() replaces one byte of the next read from that sector; shortRead() cuts
/// it to `bytes`; failReads() makes every RL answer `output` with `exitCode` (0 with "Read LBA
/// failed!" is a failure by text); noSuchDevice() answers "No found specific rockusb device!" exit 255.
/// readCommands() lists the RL lines in order; writesBefore(sector) is how many WL lines preceded
/// the RL from that sector (the ordering rule). An unaddressed RL answers "fake: unaddressed read
/// refused" with exit 99 and is counted (unaddressedReads()). hangReads() holds every RL at 10 %
/// until releaseReads() (which ends the held ones as equal reads and stops holding). Its handlers
/// match every RL with -s, the Loader-mode layout read's `RL 0 128` included: a test that needs
/// that read registers scriptLoaderLayoutRead() first. The state is shared with the handlers, so
/// the object may go before the runner.
struct FakeFlash
{
    FakeFlash();
    void script(FakeToolRunner& runner);
    void corruptByte(quint64 sector, qint64 index, quint8 value = 0x3c);
    void shortRead(quint64 sector, qint64 bytes);
    void failReads(const QByteArray& output, int exitCode);
    void noSuchDevice();
    void hangReads();
    void releaseReads();
    QStringList readCommands() const;
    QStringList writeCommands() const;
    int unaddressedReads() const;
    int writesBefore(quint64 sector) const;   ///< -1 when no RL read from `sector`

    struct State;
    std::shared_ptr<State> state;
};
/// scriptSocFlash() with its WL script replaced by the flash's WL handler, plus the RL handler (the
/// flash's handlers are registered first: the first matching handler wins).
void scriptSocFlashWithReadBack(FakeToolRunner& runner, const QByteArray& ld, FakeFlash& flash);

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
