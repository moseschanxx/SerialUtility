#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>

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
    QString chipTag;         ///< "110E"
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
    /// cancelled by SocFlasher.
    ToolInvocation* writeLba(quint64 startSector, quint64 sectorCount, const QString& file);
    ToolInvocation* resetDevice();                       ///< upgrade_tool RD ("Reset Device OK.")

    // ---- Parsers -------------------------------------------------------------------------
    static QList<RockusbDevice> parseDeviceList(const QString& output);
    static RkFirmwareInfo parseFirmwareInfo(const QString& output);
    static bool reportsSuccess(const QString& output);
    static QString stripBanner(const QString& output);   ///< without the "rockusb driver = ..." line(s)
    /// The last meaningful lines of a UF output for an error message (usb_update.sh: without
    /// "Using ..." lines, '\r' as line breaks, blank lines dropped, the last `count` lines).
    static QString tailForMessage(const QString& output, int count = 8);
    /// True when `path` starts with the RKFW magic (the 4 bytes "RKFW"); false for a missing file.
    static bool isRkfwImage(const QString& path, QString* error = nullptr);
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
    /// layout the image was built for, so SocFlasher prefers it over the board's (see there).
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
