#pragma once

#include <QObject>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

#include "firmware/ToolRunner.h"

/// One line of `upgrade_tool LD`.
struct RockusbDevice
{
    enum class Mode { Unknown, Loader, Maskrom };
    int devNo = 0;
    quint16 vid = 0;
    quint16 pid = 0;
    QString locationId;
    Mode mode = Mode::Unknown;
    QString serial;
    QString modeText() const;   ///< "Loader" / "Maskrom" / "Unknown"
    bool operator==(const RockusbDevice& other) const;
};

/// One partition of a Linux `mtdparts=` string (what the Node board's /proc/cmdline carries):
/// `sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata)`.
struct MtdPartition
{
    QString name;
    quint64 offset = 0;   ///< bytes from the start of the flash
    quint64 size = 0;     ///< bytes
    bool operator==(const MtdPartition& other) const { return name == other.name && offset == other.offset && size == other.size; }
};

/// One image of an unpacked update.img placed in the flash layout (RockchipTool::planPartitions(),
/// SocFlasher::partitions()): what the Firmware tab lists with a checkbox per row.
struct PartitionImage
{
    QString name;            ///< partition name from package-file ("env", "boot", ...)
    QString file;            ///< absolute path of the extracted image
    qint64 fileSize = -1;    ///< bytes of that file (-1 when missing)
    quint64 offset = 0;      ///< from the layout
    quint64 size = 0;        ///< partition size from the layout; 0 = "to the end" (unknown)
    bool inLayout = false;   ///< the layout has a partition of this name
    bool fits = false;       ///< fileSize <= size (always true when size == 0)
    bool selected = true;    ///< written by the next flash (the tab's checkbox)
    bool operator==(const PartitionImage& other) const;
};

/// (v0.8.0) The fixed header of an RKFW image, read from the file itself - no tool, so the image's
/// chip tag is known before any upgrade_tool runs and the command lines of a flash stay what they
/// were (RockchipTool::readRkfwHeader()). Layout, verified on the Node image of 2026-10-13 (SFI
/// prints the same values): bytes 0-3 "RKFW"; 4-5 header length LE (0x66); 6-9 version (build =
/// bytes 6-7 LE, minor = byte 8, major = byte 9: "0.0.00"); 0x0e-0x0f year LE, 0x10 month, 0x11
/// day, 0x12 hour, 0x13 minute, 0x14 second (2026-10-13 21:45:50); 0x15-0x18 the chip code as a
/// little-endian 32-bit word whose four ASCII bytes read most-significant first: bytes 45 30 31 31
/// = 0x31313045 = "110E" (the Node, RV1103B); the stacked board's rkImageMaker -RK1106 writes
/// 0x31313036 = "1106" (RV1106G2_FACTS.md row 20); 0x19-0x1c loader offset, 0x1d-0x20 loader size,
/// 0x21-0x24 image offset, 0x25-0x28 image size (all LE). Anything beyond the first 0x66 bytes is
/// not read.
struct RkfwHeader
{
    bool valid = false;      ///< the magic, the length and the chip code were read
    QString chipTag;         ///< "110E" / "1106" (upper case, four characters)
    QString version;         ///< "0.0.00" as SFI prints it
    QDateTime buildTime;     ///< the header's date and time (local, no offset)
    quint32 loaderOffset = 0;
    quint32 loaderSize = 0;
    quint32 imageOffset = 0;
    quint32 imageSize = 0;
    bool operator==(const RkfwHeader& other) const;
};

/// (v0.8.0) What a verify read-back found (RockchipTool::compareReadBack()): the first `bytes` of
/// the file read back with `RL` against the image written with `WL`.
struct ReadBackResult
{
    QString name;            ///< the partition ("rootfs"); filled by the caller
    bool ok = false;         ///< every byte of the image equals what was read back
    qint64 bytes = 0;        ///< the image's length = the compared length (never the partition's)
    qint64 mismatchOffset = -1;   ///< the first differing byte, as an offset into the image; -1 when ok or not compared
    quint8 readByte = 0;     ///< the byte read back at mismatchOffset
    quint8 imageByte = 0;    ///< the image's byte there
    qint64 milliseconds = 0; ///< the RL's duration; filled by the caller
    QString error;           ///< why it could not be compared: "short read: 1024 of 3830272 bytes", "cannot read <file>: ...", the RL's failure tail
    bool operator==(const ReadBackResult& other) const;
};

/// `upgrade_tool SFI <update.img>`.
struct RkFirmwareInfo
{
    struct Entry
    {
        int no = 0;
        QString file;        ///< "boot.img"
        QString partition;   ///< "boot"
        QString type;        ///< "image" or empty
        quint64 offset = 0;
        quint64 size = 0;
    };
    QString file;            ///< "File:..." line
    QString type;            ///< "Update Firmware"
    QString chipTag;         ///< "110E" (the Node); "1106" (the stacked RV1106G2 board, v0.8.0)
    QString version;         ///< "0.0.00"
    QString buildTime;       ///< "2026-10-13 21:45:50"
    bool signedImage = false;
    QString loaderVersion;   ///< "1.01"
    QString loaderTime;
    QList<Entry> entries;
    bool isValid() const { return !type.isEmpty(); }
    QString summary() const; ///< "Update Firmware, chip 110E, built 2026-10-13 21:45:50, loader 1.01, 9 entries"
};

/**
 * Rockchip's upgrade_tool (v2.37 Windows / v2.26 Linux; the Windows one needs no config.ini -
 * verified 2026-10-13) behind the ToolRunner, plus the parsers of its output.
 *
 * Output facts (seen on this PC): every run starts with a banner line such as
 * "rockusb driver = 5.12,please upgrade it to enjoy fast download"; `LD` prints
 * "List of rockusb connected(N)" then one line per device
 * "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=...": the parser takes
 * every line with "DevNo=" and reads the key=value pairs, Mode "Loader" / "Maskrom"
 * (case-insensitive), the rest Unknown. `SFI` prints "Type:...", "Chip Tag:110E\tVersion:0.0.00
 * \tBuild Time:...\tSign:false", "Loader ver:1.01\tLoader Time:...", "Entry Count:9" and
 * "    EntryNo=0 ;    file=env.img;    partition=env;    type=image;    offset=0x3d25f;
 * size=0x10000" lines. `UF` prints stages separated by carriage returns, e.g.
 * "Loading loader...", "Download Boot Start", "Download Boot Success", "Wait For Maskrom Start",
 * "Download Image... (42%)", and ends with "Upgrade firmware ok." on success or an error text
 * ("Creating Comm Object failed!", "Upgrade firmware fail", ...).
 *
 * parseProgress() takes the raw bytes as they arrive (chunks may split anywhere), keeps the
 * partial tail between calls and reports every completed segment (split at '\r' and '\n') as a
 * stage text and, when the segment carries "(nn%)", the percentage; a segment without a
 * percentage is a stage change with percent -1. reportsSuccess() is true once the output contains
 * "Upgrade firmware ok" (case-insensitive, like usb_update.sh's grep -qi).
 *
 * Addressed commands (v0.7.0, the fleet flash; docs/DESIGN.md 4.13): `upgrade_tool LD` lists every
 * rockusb device with a LocationID the tool assigns per USB location ("DevNo=1\tVid=0x2207,
 * Pid=0x110e,LocationID=112\tMode=Loader\tSerialNo=" - 112 and 154 on this PC; its encoding is
 * not documented and is never decoded, only learned from `LD`), and `upgrade_tool -s <LocationID>
 * <command>` addresses exactly that device, which is what SocToolKit does with every command when
 * several boards are in download mode (its device string is "<mode> <serialno> <locationid>").
 * Measured 2026-10-14: a wrong id answers "No found specific rockusb device!" with exit 255;
 * WITHOUT `-s` a WL / RD with two devices present silently takes one of them (exit 0) - the hazard
 * the owner's rule names, so the fleet never issues an unaddressed write. writeLbaOn() /
 * resetDeviceOn() carry the id in their signature: there is no state on this object that could
 * leak an address into, or drop it from, a command. The plain listDevices() / firmwareInfo() /
 * extractFirmware() / upgradeFirmware() / writeLba() / resetDevice() are untouched (the 0.6.0
 * single-board path is byte for byte what it was); `LD` lists every device and takes no `-s`,
 * `EXF` and `SFI` read a file and take none either.
 *
 * Reads from the board (v0.8.0; docs/DESIGN.md 4.15 / 4.16), ALL addressed - a read with several
 * devices in download mode has the same hazard as a write, and the owner's rule is "every RL
 * carries -s": readLbaOn() is `upgrade_tool -s <id> RL <startSector> <sectorCount> <file>`, the
 * read-back of WL (the vendor's rkdownload.sh reads the env partition with `RL 0 1 /tmp/env.img`):
 * the layout read of a board met in Loader mode - the FIRST source SocFlasher tries there, the
 * vendor's own way - reads the first kEnvPartitionSectors (64 KiB: the env partition starts at
 * sector 0 on both boards, row 18) and parses it with mtdPartsFromEnvImage(); the verify step reads
 * ceil(imageSize / 512) sectors from the row's sector and compareReadBack() compares the image's
 * length - never the partition's, so a jffs2 / erofs image padded on the flash compares over its
 * own bytes. partitionListOn() is `upgrade_tool -s <id> PL`, the tool's partition table of the
 * device, the FALLBACK when the env partition holds no layout: its output on a NOR board whose
 * table lives in env.img is UNVERIFIED (open question 7 of RV1106G2_FACTS.md; parsePartitionList()
 * is written for the documented shape and refuses anything else, so a tool default never becomes
 * "the board's layout" - the raw output is logged for the first G2 to answer the question). An RL
 * is never started while a WL of the same device is in flight (the callers' rule: SocFlasher
 * writes one row at a time, the fleet keeps one process per board) and is counted like a WL for
 * the fleet's LD gate. The image's chip tag comes from readRkfwHeader(), a file read: no tool runs
 * for it.
 */
class RockchipTool : public QObject
{
    Q_OBJECT
public:
    struct Progress
    {
        QString stage;       ///< "Download Image..." (the text before the percentage, trimmed)
        int percent = -1;    ///< -1 when the segment has none
        bool operator==(const Progress& other) const { return stage == other.stage && percent == other.percent; }
    };

    explicit RockchipTool(ToolRunner* runner, QObject* parent = nullptr);

    void setToolPath(const QString& path);   ///< ToolLocator::locate(Tool::UpgradeTool) by the owner
    QString toolPath() const;
    void setTimeoutMs(int ms);               ///< LD / SFI timeout, default 15000; UF has none
    int timeoutMs() const;
    ToolRunner* runner() const;

    ToolInvocation* listDevices();                       ///< upgrade_tool LD
    /// The image commands pass the path with native separators and run in the image's folder
    /// (Options::workingDirectory), a writable place for whatever upgrade_tool writes next to it.
    ToolInvocation* firmwareInfo(const QString& image);  ///< upgrade_tool SFI <image>
    ToolInvocation* upgradeFirmware(const QString& image);   ///< upgrade_tool UF <image>; never cancelled by SocFlasher
    /// upgrade_tool EXF <image> <outDir>: unpacks the RKFW package into its partition images plus
    /// `package-file` ("<partition>\t<file>" lines) and `bootloader.bin`; prints
    /// "start to extract <file>...(nn%)" lines and "Extract ok." (v2.37).
    ToolInvocation* extractFirmware(const QString& image, const QString& outDir);
    /// upgrade_tool WL <startSector> <sectorCount> <file>: write a file to the flash from a 512-byte
    /// sector; `sectorCount` = the partition's size in sectors (0 = "to the end", passed as -1), the
    /// command SocToolKit's Download page runs for every partition of a Loader-mode board
    /// (`wl <offset / 512> <size / 512> <file>`; progress lines "Write LBA from file (nn%)"; no
    /// chip check). The size is required: without it upgrade_tool v2.37 refuses a jffs2 image
    /// ("Image is jffs format,please tell the size!" for userdata.img, seen 2026-10-13). Never
    /// cancelled by SocFlasher. UNADDRESSED: with two devices in download mode upgrade_tool
    /// silently takes one of them (measured 2026-10-14), and SocFlasher's detection takes the first
    /// Loader device `LD` lists - so the single-board flow is right only while `LD` lists ONE
    /// device; a fleet board left in Loader mode is retried by hand through `-s` (the retry lines
    /// FleetFlasher logs), never through `su-fw flash-soc --serial` while another device is in
    /// download mode. (v0.8.0: SocFlasher enforces this - when its `LD` lists more than one device
    /// in download mode the flow refuses before EXF, PL / RL or any WL, SocFlasher.h "Several
    /// devices in download mode"; this command itself is unchanged.)
    ToolInvocation* writeLba(quint64 startSector, quint64 sectorCount, const QString& file);
    ToolInvocation* resetDevice();                       ///< upgrade_tool RD ("Reset Device OK."); unaddressed, as writeLba()

    // ---- Addressed commands (v0.7.0, the fleet): `upgrade_tool -s <LocationID> <command>` ------
    /// `upgrade_tool -s <locationId> WL <startSector> <sectorCount> <file>`: writeLba() for one
    /// device among several - `-s <id>` right before the command, as SocToolKit passes it
    /// (`upgrade_tool -s 112 WL 8064 10240 rootfs.img`); the count rule (-1 for 0) and the native
    /// path as writeLba(). No timeout, never cancelled, like writeLba(). The command runs in
    /// `workingDirectory` when given, else in the file's folder: upgrade_tool writes a Log folder
    /// into its working directory, and N processes must not share one (FleetFlasher gives every
    /// board a folder of its own). `locationId` must not be empty: Q_ASSERT in a debug build, and
    /// in a release build NOTHING runs and null is returned (the only method of this class that
    /// can return null) - the caller fails the board instead of writing it; the fleet's tests
    /// assert that no WL ever leaves without `-s`.
    ToolInvocation* writeLbaOn(const QString& locationId, quint64 startSector, quint64 sectorCount, const QString& file,
                               const QString& workingDirectory = QString());
    /// `upgrade_tool -s <locationId> RD`: resetDevice() for one device among several (the LD
    /// timeout applies, as for resetDevice()). An empty id: Q_ASSERT, and null in a release build
    /// (nothing runs), as writeLbaOn().
    ToolInvocation* resetDeviceOn(const QString& locationId);

    // ---- Addressed reads (v0.8.0: layout-pl implements partitionListOn() / readLbaOn(); verify-rl uses readLbaOn()) ----
    static constexpr quint64 kEnvPartitionSectors = 128;   ///< 64 KiB / 512: the env partition at sector 0 on both boards
    /// `upgrade_tool -s <locationId> PL`: the partition table the tool reads from the device, with
    /// the LD timeout. The same empty-id rule as writeLbaOn(): Q_ASSERT, and null in a release build.
    ToolInvocation* partitionListOn(const QString& locationId);
    /// `upgrade_tool -s <locationId> RL <startSector> <sectorCount> <file>`: read sectors from the
    /// flash into `file` (native path; its folder is created by the caller). No timeout, never
    /// cancelled, like a WL - a cut RL is harmless, a timed-out one would call a slow read "not
    /// verified"; the callers log the silence line as for a WL. Runs in `workingDirectory` when
    /// given, else in the file's folder (upgrade_tool's Log folder, as writeLbaOn()). The same
    /// empty-id rule as writeLbaOn(); `sectorCount` 0 is refused the same way (null): a read "to
    /// the end" has no meaning here.
    ToolInvocation* readLbaOn(const QString& locationId, quint64 startSector, quint64 sectorCount, const QString& file,
                              const QString& workingDirectory = QString());

    // ---- Parsers -------------------------------------------------------------------------
    static QList<RockusbDevice> parseDeviceList(const QString& output);
    static RkFirmwareInfo parseFirmwareInfo(const QString& output);
    static bool reportsSuccess(const QString& output);
    /// (v0.7.0) The device `LD` listed at `locationId` (parseDeviceList()'s entries), std::nullopt
    /// when none has that id.
    static std::optional<RockusbDevice> deviceAt(const QList<RockusbDevice>& devices, const QString& locationId);
    /// (v0.7.0) upgrade_tool's answer to a `-s <id>` that matches no device: the output carries
    /// "No found specific rockusb device" (case-insensitive; exit 255 comes with it). The id is
    /// gone (the board left Loader mode, or re-enumerated with another id): nothing was written.
    static bool reportsNoSuchDevice(const QString& output);
    /// (v0.7.0; hoisted from SocFlasher.cpp, behaviour unchanged) A WL that went wrong says so:
    /// "Write LBA failed!", "Error:...->Fail to write lba,...", "Write LBA quit, creating comm
    /// object failed!" (the texts of v2.37) - `\bfail`, `\bquit\b` or `\berror\b`, case-insensitive,
    /// in the output without its banner lines (stripBanner() is applied here: "please upgrade it to
    /// enjoy fast download" has none of these words either way). Its exit status has not been seen
    /// for every failure, so the text counts as well; SocFlasher and FleetFlasher share this one
    /// definition of a failed WL.
    static bool reportsWriteFailure(const QString& output);
    /// (v0.7.0; hoisted from SocFlasher.cpp, behaviour unchanged) `RD` reported "Reset Device OK"
    /// (case-insensitive). A reset counts as done when the command exited 0 or printed this: the
    /// one rule SocFlasher's resetBoard() and FleetFlasher use.
    static bool reportsResetOk(const QString& output);
    static QString stripBanner(const QString& output);   ///< without the "rockusb driver = ..." line(s)
    /// The last meaningful lines of a UF output for an error message (usb_update.sh: without
    /// "Using ..." lines, '\r' as line breaks, blank lines dropped, the last `count` lines).
    static QString tailForMessage(const QString& output, int count = 8);
    /// True when `path` starts with the RKFW magic (the 4 bytes "RKFW"); false for a missing file.
    static bool isRkfwImage(const QString& path, QString* error = nullptr);
    /// (v0.8.0) The RkfwHeader of `path` (the layout in its comment): valid when the file starts
    /// with "RKFW" and holds at least 0x29 bytes; otherwise valid = false with *error ("file not
    /// found", "not a Rockchip RKFW image", "header truncated"). chipTag is the four ASCII bytes
    /// of the chip code read most-significant first ("110E"); a code with a non-printable byte
    /// gives the hex word instead ("0x31313045") so nothing is ever guessed.
    static RkfwHeader readRkfwHeader(const QString& path, QString* error = nullptr);
    /// (v0.8.0, layout-pl; UNVERIFIED on NOR boards, open question 7) `upgrade_tool PL`'s output
    /// as partitions in bytes. Accepted lines: an optional index, then an offset and a size (hex
    /// "0x..." or decimal, in 512-byte sectors - the unit WL takes) and a name of [A-Za-z0-9_-]+,
    /// in either order "offset size name" or "name offset size"; the banner, "List of ..." and
    /// header lines are skipped; a size of 0 or "-" is "to the end" (size 0). The result must have
    /// at least two entries with strictly ascending offsets and the first at sector 0 (the env on
    /// both boards); anything else - garbage, a header only, a table that is not the NOR's - is an
    /// empty list, and the caller falls back to the env read.
    static QList<MtdPartition> parsePartitionList(const QString& output);
    /// (v0.8.0, verify-rl) Compare `readBackFile` with `imageFile` over the image's length: ok
    /// when every byte equals; a shorter read-back is error "short read: <n> of <m> bytes"
    /// (mismatchOffset -1); the first differing byte fills mismatchOffset / readByte / imageByte;
    /// a file that cannot be opened is error "cannot read <native path>: <reason>". Streamed in
    /// 1 MiB blocks (a 12 MiB boot image is compared without loading it). `name` and
    /// `milliseconds` are left for the caller.
    static ReadBackResult compareReadBack(const QString& imageFile, const QString& readBackFile);
    /// Parse a Linux mtdparts string: the whole "mtdparts=<dev>:<list>" word, "<dev>:<list>" or
    /// the bare list. Each entry is "<size>(<name>)" or "<size>@<offset>(<name>)"; sizes and
    /// offsets take a K / M / G suffix (1024-based) or are plain bytes; an entry without "@"
    /// starts where the previous one ended; "-" as a size means "to the end" (size 0 here).
    /// Names are returned as written. An empty list when nothing parses.
    static QList<MtdPartition> parseMtdParts(const QString& text);
    /// The mtdparts word of a kernel command line ("... mtdparts=sfc_nor:64K(env),... rk_dma_heap_cma=72M")
    /// without the "mtdparts=" prefix, or an empty string when absent.
    static QString mtdPartsFromCommandLine(const QString& cmdline);
    /// The layout an image carries: `env.img` of an unpacked update.img is a U-Boot environment
    /// blob (a 4-byte CRC, then NUL-separated "key=value" entries) whose first entry is
    /// "mtdparts=<dev>:<list>" on the Node board. Returns the value without the "mtdparts="
    /// prefix, or an empty string when the file is missing or has no such entry. This is the
    /// layout the image was built for, so SocFlasher prefers it over the board's (see there)
    /// (v0.8.0: refused unless forced - rule 2 in SocFlasher.h; the same parser reads the env
    /// partition a Loader-met board's `RL 0 128` brings back).
    static QString mtdPartsFromEnvImage(const QString& envImagePath);
    /// `package-file` of an extracted image: one "<partition>\t<file>" (or space separated) per
    /// line; blank lines and lines starting with '#' skipped. First = partition name, second = file.
    static QList<QPair<QString, QString>> parsePackageFile(const QString& text);
    /// The rows of an extracted image placed in `layout`: every package-file entry in order except
    /// "package-file" and "bootloader" (upgrade_tool's own entries), with the file's size, the
    /// layout's offset / size, inLayout and fits computed; selected = inLayout && fits. An empty
    /// list (with *error) when package-file is missing or unreadable.
    static QList<PartitionImage> planPartitions(const QString& extractDir, const QList<MtdPartition>& layout,
                                                QString* error = nullptr);

    /// Incremental UF progress parser (see the class comment).
    class ProgressParser
    {
    public:
        QList<Progress> feed(const QByteArray& chunk);
        QList<Progress> flush();      ///< the partial tail, if any, as a final segment
        QString lastStage() const { return m_lastStage; }
        int lastPercent() const { return m_lastPercent; }
    private:
        QByteArray m_tail;
        QString m_lastStage;
        int m_lastPercent = -1;
    };

private:
    ToolRunner* m_runner = nullptr;
    QString m_tool;
    int m_timeoutMs = 15000;
};
