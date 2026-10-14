#include "firmware/RockchipTool.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>

#include <algorithm>
#include <limits>

namespace {

/// The tool's first line on this PC: "rockusb driver = 5.12,please upgrade it to enjoy fast download".
bool isBannerLine(const QString& line)
{
    static const QRegularExpression banner(QStringLiteral("^\\s*rockusb driver\\b"),
                                           QRegularExpression::CaseInsensitiveOption);
    return banner.match(line).hasMatch();
}

/// Output text split into lines at "\r\n", "\r" or "\n" (upgrade_tool uses all three), empty
/// lines included.
QStringList splitLines(const QString& text)
{
    static const QRegularExpression terminator(QStringLiteral("\\r\\n|\\r|\\n"));
    return text.split(terminator);
}

/// "0x3d25f" -> 0x3d25f, "1234" -> 1234 (an unprefixed number is decimal here: sizes and offsets).
quint64 parseNumber(const QString& text, bool* ok = nullptr)
{
    const QString t = text.trimmed();
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        return t.mid(2).toULongLong(ok, 16);
    }
    return t.toULongLong(ok, 10);
}

/// A USB id as `upgrade_tool` prints it ("0x2207"); an unprefixed number is read as hex, which is
/// how USB ids are always written.
quint16 parseUsbId(const QString& text)
{
    QString t = text.trimmed();
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        t = t.mid(2);
    }
    bool ok = false;
    const uint value = t.toUInt(&ok, 16);
    return ok && value <= 0xFFFF ? static_cast<quint16>(value) : 0;
}

QString regexCapture(const QString& line, const char* pattern)
{
    const QRegularExpression re(QString::fromLatin1(pattern), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

/// One "DevNo=1<TAB>Vid=0x2207,Pid=0x110e,LocationID=104<TAB>Mode=Loader<TAB>SerialNo=" line.
/// The keys are looked up one by one so tabs, commas and spaces all work as separators.
RockusbDevice parseDeviceLine(const QString& line)
{
    RockusbDevice device;
    device.devNo = regexCapture(line, "\\bDevNo\\s*=\\s*(\\d+)").toInt();
    device.vid = parseUsbId(regexCapture(line, "\\bVid\\s*=\\s*(?:0x)?([0-9a-f]+)"));
    device.pid = parseUsbId(regexCapture(line, "\\bPid\\s*=\\s*(?:0x)?([0-9a-f]+)"));
    device.locationId = regexCapture(line, "\\bLocationID\\s*=\\s*([^\\s,]+)");
    device.serial = regexCapture(line, "\\bSerialNo\\s*=\\s*([^\\s,]*)");
    const QString mode = regexCapture(line, "\\bMode\\s*=\\s*([a-z]+)");
    if (mode.compare(QLatin1String("Loader"), Qt::CaseInsensitive) == 0) {
        device.mode = RockusbDevice::Mode::Loader;
    } else if (mode.compare(QLatin1String("Maskrom"), Qt::CaseInsensitive) == 0) {
        device.mode = RockusbDevice::Mode::Maskrom;
    }
    return device;
}

/// "    EntryNo=7 ;    file=package-file;    partition=userdata;    type=;    offset=0xa96a5f;    size=0xa7"
RkFirmwareInfo::Entry parseEntryLine(const QString& line)
{
    RkFirmwareInfo::Entry entry;
    const QStringList pieces = line.split(QLatin1Char(';'));
    for (const QString& piece : pieces) {
        const qsizetype eq = piece.indexOf(QLatin1Char('='));
        if (eq < 0) {
            continue;
        }
        const QString key = piece.left(eq).trimmed().toLower();
        const QString value = piece.mid(eq + 1).trimmed();
        if (key == QLatin1String("entryno")) {
            entry.no = value.toInt();
        } else if (key == QLatin1String("file")) {
            entry.file = value;
        } else if (key == QLatin1String("partition")) {
            entry.partition = value;
        } else if (key == QLatin1String("type")) {
            entry.type = value;
        } else if (key == QLatin1String("offset")) {
            entry.offset = parseNumber(value);
        } else if (key == QLatin1String("size")) {
            entry.size = parseNumber(value);
        }
    }
    return entry;
}

/// The "Key:value" fields of one SFI line. Several fields may share a line ("Chip Tag:110E<TAB>
/// Version:0.0.00<TAB>Build Time:2026-10-13 21:45:50<TAB>Sign:false"), separated by tabs or spaces,
/// and a value may hold spaces (the build time), so a value runs to the next known key.
QList<QPair<QString, QString>> parseFields(const QString& line)
{
    static const QRegularExpression key(
        QStringLiteral("(File|Type|Chip Tag|Version|Build Time|Loader ver|Loader Time|Loader Sign|Sign|Entry Count)\\s*:"),
        QRegularExpression::CaseInsensitiveOption);
    QList<QPair<QString, QString>> fields;
    QRegularExpressionMatchIterator it = key.globalMatch(line);
    QRegularExpressionMatch current;
    while (it.hasNext()) {
        const QRegularExpressionMatch next = it.next();
        if (current.hasMatch()) {
            fields.append({current.captured(1).toLower(),
                           line.mid(current.capturedEnd(0), next.capturedStart(0) - current.capturedEnd(0)).trimmed()});
        }
        current = next;
    }
    if (current.hasMatch()) {
        fields.append({current.captured(1).toLower(), line.mid(current.capturedEnd(0)).trimmed()});
    }
    return fields;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------------------------

QString RockusbDevice::modeText() const
{
    switch (mode) {
    case Mode::Loader: return QStringLiteral("Loader");
    case Mode::Maskrom: return QStringLiteral("Maskrom");
    case Mode::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

bool RockusbDevice::operator==(const RockusbDevice& o) const
{
    return devNo == o.devNo && vid == o.vid && pid == o.pid && locationId == o.locationId && mode == o.mode
           && serial == o.serial;
}

bool PartitionImage::operator==(const PartitionImage& o) const
{
    return name == o.name && file == o.file && fileSize == o.fileSize && offset == o.offset && size == o.size
           && inLayout == o.inLayout && fits == o.fits && selected == o.selected;
}

QString RkFirmwareInfo::summary() const
{
    if (!isValid()) {
        return QString();
    }
    QStringList parts{type};
    if (!chipTag.isEmpty()) {
        parts << QCoreApplication::translate("RkFirmwareInfo", "chip %1").arg(chipTag);
    }
    if (!buildTime.isEmpty()) {
        parts << QCoreApplication::translate("RkFirmwareInfo", "built %1").arg(buildTime);
    }
    if (!loaderVersion.isEmpty()) {
        parts << QCoreApplication::translate("RkFirmwareInfo", "loader %1").arg(loaderVersion);
    }
    if (!entries.isEmpty()) {
        // Two plain strings instead of a %n plural: "1 entry" must read right without a translator.
        parts << (entries.size() == 1
                      ? QCoreApplication::translate("RkFirmwareInfo", "1 entry")
                      : QCoreApplication::translate("RkFirmwareInfo", "%1 entries").arg(entries.size()));
    }
    return parts.join(QStringLiteral(", "));
}

// ---------------------------------------------------------------------------------------------
// RockchipTool
// ---------------------------------------------------------------------------------------------

RockchipTool::RockchipTool(ToolRunner* runner, QObject* parent) : QObject(parent), m_runner(runner) {}
void RockchipTool::setToolPath(const QString& path) { m_tool = path; }
QString RockchipTool::toolPath() const { return m_tool; }
void RockchipTool::setTimeoutMs(int ms) { m_timeoutMs = ms; }
int RockchipTool::timeoutMs() const { return m_timeoutMs; }
ToolRunner* RockchipTool::runner() const { return m_runner; }

ToolInvocation* RockchipTool::listDevices()
{
    ToolRunner::Options options;
    options.timeoutMs = m_timeoutMs;
    return m_runner->run(m_tool, {QStringLiteral("LD")}, options);
}

namespace {

/// The image commands run with the image path in native separators (upgrade_tool does its own
/// path handling: it derives folders for its log and the loader extraction, and its Windows
/// build has only been seen with backslash paths) and in the image's folder (a writable one:
/// the tools folder of an installed copy is not, and upgrade_tool may write its Log folder into
/// the working directory).
ToolRunner::Options imageCommandOptions(const QString& image, int timeoutMs)
{
    ToolRunner::Options options;
    options.timeoutMs = timeoutMs;
    if (!image.isEmpty()) {
        options.workingDirectory = QFileInfo(image).absolutePath();
    }
    return options;
}

} // namespace

ToolInvocation* RockchipTool::firmwareInfo(const QString& image)
{
    return m_runner->run(m_tool, {QStringLiteral("SFI"), QDir::toNativeSeparators(image)},
                         imageCommandOptions(image, m_timeoutMs));
}

ToolInvocation* RockchipTool::upgradeFirmware(const QString& image)
{
    // No timeout: UF writes the flash, and killing it half way is what the owner must never do.
    return m_runner->run(m_tool, {QStringLiteral("UF"), QDir::toNativeSeparators(image)}, imageCommandOptions(image, 0));
}

namespace {

/// EXF only reads the package and writes files on this PC, so it may be stopped; the limit is
/// generous because an eMMC image of a few GB takes minutes (the Node board's 11 MB take two seconds).
constexpr int kExtractTimeoutMs = 10 * 60 * 1000;

} // namespace

ToolInvocation* RockchipTool::extractFirmware(const QString& image, const QString& outDir)
{
    // upgrade_tool joins "<outDir><separator><file>" itself (its "%s%cbootloader.bin" format), so
    // the folder goes without a trailing separator; it does not create the folder.
    QDir().mkpath(outDir);
    return m_runner->run(m_tool, {QStringLiteral("EXF"), QDir::toNativeSeparators(image), QDir::toNativeSeparators(outDir)},
                         imageCommandOptions(image, kExtractTimeoutMs));
}

ToolInvocation* RockchipTool::writeLba(quint64 startSector, quint64 sectorCount, const QString& file)
{
    // No timeout, like UF: a WL stopped half way leaves a partition half written.
    // The count is what SocToolKit passes: the partition size in sectors, -1 for "to the end"
    // (upgrade_tool needs it to lay out a jffs2 image over the whole partition).
    const QString count = sectorCount == 0 ? QStringLiteral("-1") : QString::number(sectorCount);
    return m_runner->run(m_tool,
                         {QStringLiteral("WL"), QString::number(startSector), count, QDir::toNativeSeparators(file)},
                         imageCommandOptions(file, 0));
}

ToolInvocation* RockchipTool::resetDevice()
{
    ToolRunner::Options options;
    options.timeoutMs = m_timeoutMs;
    return m_runner->run(m_tool, {QStringLiteral("RD")}, options);
}

ToolInvocation* RockchipTool::writeLbaOn(const QString& locationId, quint64 startSector, quint64 sectorCount,
                                         const QString& file, const QString& workingDirectory)
{
    // The address is part of the call: an empty one is a programming error, and a release build
    // writes nothing rather than an unaddressed WL that would take any device in download mode.
    Q_ASSERT(!locationId.isEmpty());
    if (locationId.isEmpty()) {
        return nullptr;
    }
    const QString count = sectorCount == 0 ? QStringLiteral("-1") : QString::number(sectorCount);
    ToolRunner::Options options = imageCommandOptions(file, 0);   // no timeout: a WL is never cut
    if (!workingDirectory.isEmpty()) {
        options.workingDirectory = workingDirectory;
    }
    return m_runner->run(m_tool,
                         {QStringLiteral("-s"), locationId, QStringLiteral("WL"), QString::number(startSector), count,
                          QDir::toNativeSeparators(file)},
                         options);
}

ToolInvocation* RockchipTool::resetDeviceOn(const QString& locationId)
{
    Q_ASSERT(!locationId.isEmpty());
    if (locationId.isEmpty()) {
        return nullptr;
    }
    ToolRunner::Options options;
    options.timeoutMs = m_timeoutMs;   // a timed-out RD is a failed reset, never a cut write
    return m_runner->run(m_tool, {QStringLiteral("-s"), locationId, QStringLiteral("RD")}, options);
}

QList<RockusbDevice> RockchipTool::parseDeviceList(const QString& output)
{
    QList<RockusbDevice> devices;
    for (const QString& line : splitLines(output)) {
        if (line.contains(QLatin1String("DevNo"), Qt::CaseInsensitive)
            && line.contains(QLatin1Char('='))) {
            devices.append(parseDeviceLine(line));
        }
    }
    return devices;
}

RkFirmwareInfo RockchipTool::parseFirmwareInfo(const QString& output)
{
    RkFirmwareInfo info;
    for (const QString& rawLine : splitLines(output)) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || isBannerLine(line)) {
            continue;
        }
        if (line.startsWith(QLatin1String("EntryNo"), Qt::CaseInsensitive)) {
            info.entries.append(parseEntryLine(line));
            continue;
        }
        for (const auto& [key, value] : parseFields(line)) {
            if (key == QLatin1String("file")) {
                info.file = value;
            } else if (key == QLatin1String("type")) {
                info.type = value;
            } else if (key == QLatin1String("chip tag")) {
                info.chipTag = value;
            } else if (key == QLatin1String("version")) {
                info.version = value;
            } else if (key == QLatin1String("build time")) {
                info.buildTime = value;
            } else if (key == QLatin1String("sign")) {
                info.signedImage = value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
            } else if (key == QLatin1String("loader ver")) {
                info.loaderVersion = value;
            } else if (key == QLatin1String("loader time")) {
                info.loaderTime = value;
            }
            // "Loader Sign" and "Entry Count" carry nothing the tab shows: the entries are counted.
        }
    }
    return info;
}

bool RockchipTool::reportsSuccess(const QString& output)
{
    // usb_update.sh: grep -qi 'Upgrade firmware ok'
    return output.contains(QLatin1String("Upgrade firmware ok"), Qt::CaseInsensitive);
}

std::optional<RockusbDevice> RockchipTool::deviceAt(const QList<RockusbDevice>& devices, const QString& locationId)
{
    for (const RockusbDevice& device : devices) {
        if (!locationId.isEmpty() && device.locationId == locationId) {
            return device;
        }
    }
    return std::nullopt;
}

bool RockchipTool::reportsNoSuchDevice(const QString& output)
{
    return output.contains(QLatin1String("No found specific rockusb device"), Qt::CaseInsensitive);
}

bool RockchipTool::reportsWriteFailure(const QString& output)
{
    // "Write LBA failed!", "Error:...->Fail to write lba,...", "Write LBA quit, creating comm object
    // failed!" (upgrade_tool v2.37). The banner ("please upgrade it to enjoy fast download") has
    // none of these words; it is cut first all the same.
    static const QRegularExpression failure(QStringLiteral("\\bfail|\\bquit\\b|\\berror\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    return failure.match(stripBanner(output)).hasMatch();
}

bool RockchipTool::reportsResetOk(const QString& output)
{
    return output.contains(QLatin1String("Reset Device OK"), Qt::CaseInsensitive);
}

QString RockchipTool::stripBanner(const QString& output)
{
    // Keep the rest byte for byte (progress lines depend on their "\r"): cut whole banner lines
    // including their terminator.
    QString result;
    result.reserve(output.size());
    qsizetype pos = 0;
    while (pos < output.size()) {
        qsizetype end = pos;
        while (end < output.size() && output.at(end) != QLatin1Char('\r') && output.at(end) != QLatin1Char('\n')) {
            ++end;
        }
        qsizetype next = end;
        if (next < output.size()) {
            if (output.at(next) == QLatin1Char('\r') && next + 1 < output.size()
                && output.at(next + 1) == QLatin1Char('\n')) {
                next += 2;
            } else {
                next += 1;
            }
        }
        if (!isBannerLine(output.mid(pos, end - pos))) {
            result += output.mid(pos, next - pos);
        }
        pos = next;
    }
    return result;
}

QString RockchipTool::tailForMessage(const QString& output, int count)
{
    // usb_update.sh: grep -v '^Using ' | tr '\r' '\n' | grep -v '^\s*$' | tail -n 8
    QStringList lines;
    for (const QString& line : splitLines(output)) {
        if (line.trimmed().isEmpty() || line.startsWith(QLatin1String("Using ")) || isBannerLine(line)) {
            continue;
        }
        lines << line;
    }
    if (count <= 0) {
        return QString();
    }
    if (lines.size() > count) {
        lines = lines.mid(lines.size() - count);
    }
    return lines.join(QLatin1Char('\n'));
}

bool RockchipTool::isRkfwImage(const QString& path, QString* error)
{
    if (error) {
        error->clear();
    }
    if (path.isEmpty()) {
        if (error) {
            *error = QCoreApplication::translate("RockchipTool", "no firmware image selected");
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.exists()
                         ? QCoreApplication::translate("RockchipTool", "cannot read %1: %2")
                               .arg(QDir::toNativeSeparators(path), file.errorString())
                         : QCoreApplication::translate("RockchipTool", "no firmware image at %1")
                               .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    const QByteArray magic = file.read(4);
    if (magic != QByteArrayLiteral("RKFW")) {
        if (error) {
            *error = QCoreApplication::translate("RockchipTool", "%1 is not a Rockchip update image (the RKFW magic is missing)")
                         .arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Partition layout (mtdparts) and the unpacked image (package-file)
// ---------------------------------------------------------------------------------------------

namespace {

/// A size or an offset of an mtdparts entry as the kernel's memparse() reads it: decimal or 0x
/// hex, optionally followed by K / M / G (1024-based, either case). False for anything else and
/// for a value that does not fit 64 bits.
bool parseMemSize(const QString& text, quint64* value)
{
    static const QRegularExpression pattern(QStringLiteral("^(0[xX][0-9a-fA-F]+|\\d+)([kKmMgG]?)$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch()) {
        return false;
    }
    const QString digits = match.captured(1);
    bool ok = false;
    const quint64 number = digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? digits.mid(2).toULongLong(&ok, 16)
                                                                                         : digits.toULongLong(&ok, 10);
    if (!ok) {
        return false;
    }
    const QString suffix = match.captured(2).toUpper();
    const int shift = suffix == QLatin1String("K")   ? 10
                      : suffix == QLatin1String("M") ? 20
                      : suffix == QLatin1String("G") ? 30
                                                     : 0;
    if (shift > 0 && number > (std::numeric_limits<quint64>::max() >> shift)) {
        return false;
    }
    *value = number << shift;
    return true;
}

/// The entries of an mtdparts list split at the commas that are not inside a "(name)".
QStringList splitMtdEntries(const QString& list)
{
    QStringList entries;
    int depth = 0;
    qsizetype start = 0;
    for (qsizetype i = 0; i < list.size(); ++i) {
        const QChar c = list.at(i);
        if (c == QLatin1Char('(')) {
            ++depth;
        } else if (c == QLatin1Char(')')) {
            depth = qMax(0, depth - 1);
        } else if (c == QLatin1Char(',') && depth == 0) {
            entries << list.mid(start, i - start);
            start = i + 1;
        }
    }
    entries << list.mid(start);
    return entries;
}

} // namespace

QList<MtdPartition> RockchipTool::parseMtdParts(const QString& text)
{
    // <size>[@<offset>](<name>)[ro][lk][slc], the grammar of drivers/mtd/parsers/cmdlinepart.c.
    static const QRegularExpression entryPattern(
        QStringLiteral("^\\s*([^@()]+?)\\s*(?:@\\s*([^@()]+?)\\s*)?\\(([^()]*)\\)\\s*(?:ro|lk|slc)*\\s*$"));
    QString list = text.trimmed();
    if (list.startsWith(QLatin1String("mtdparts="))) {
        list = list.mid(9);
    }
    // Several flash devices ("<dev>:<list>;<dev>:<list>"): the first one is the board's boot flash.
    const qsizetype semicolon = list.indexOf(QLatin1Char(';'));
    if (semicolon >= 0) {
        list = list.left(semicolon);
    }
    // The device id ("sfc_nor:") ends at the first colon that comes before any "(name)".
    const qsizetype colon = list.indexOf(QLatin1Char(':'));
    const qsizetype paren = list.indexOf(QLatin1Char('('));
    if (colon >= 0 && (paren < 0 || colon < paren)) {
        list = list.mid(colon + 1);
    }
    if (list.trimmed().isEmpty()) {
        return {};
    }

    // Anything malformed empties the whole list: every offset depends on the entries before it,
    // and a wrong offset would make WL overwrite another partition.
    QList<MtdPartition> partitions;
    quint64 next = 0;          // where an entry without "@" starts
    bool toTheEnd = false;     // a "-" entry was seen: nothing may follow it
    for (const QString& entry : splitMtdEntries(list)) {
        const QRegularExpressionMatch match = entryPattern.match(entry);
        if (!match.hasMatch() || toTheEnd || match.captured(3).trimmed().isEmpty()) {
            return {};
        }
        MtdPartition partition;
        partition.name = match.captured(3);
        partition.offset = next;
        if (!match.captured(2).isEmpty()) {
            quint64 offset = 0;
            if (!parseMemSize(match.captured(2), &offset) || offset < next) {
                return {};   // overlapping the previous partition
            }
            partition.offset = offset;
        }
        const QString sizeText = match.captured(1).trimmed();
        if (sizeText == QLatin1String("-")) {
            partition.size = 0;
            toTheEnd = true;
        } else {
            quint64 size = 0;
            if (!parseMemSize(sizeText, &size) || size == 0 || size > std::numeric_limits<quint64>::max() - partition.offset) {
                return {};
            }
            partition.size = size;
            next = partition.offset + size;
        }
        partitions.append(partition);
    }
    return partitions;
}

QString RockchipTool::mtdPartsFromCommandLine(const QString& cmdline)
{
    // The kernel takes the last of repeated parameters, so does this.
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QString found;
    for (const QString& word : cmdline.split(whitespace, Qt::SkipEmptyParts)) {
        if (word.startsWith(QLatin1String("mtdparts="))) {
            found = word.mid(9);
        }
    }
    return found.trimmed();
}

QString RockchipTool::mtdPartsFromEnvImage(const QString& envImagePath)
{
    // A U-Boot environment blob: a CRC32 (4 bytes, ignored here), with a redundant environment one
    // more flag byte, then "key=value" entries each ending in a NUL, "\0\0" (or the padding) after
    // the last. The Node image: "mtdparts=sfc_nor:...", then "sys_bootargs= ... mtdparts=...
    // ...", "sd_parts=..." - only an entry that itself starts with "mtdparts=" counts.
    constexpr qint64 kMaxEnvBytes = 4 * 1024 * 1024;   // the Node env is 64 KiB
    static const QByteArray key("mtdparts=");
    QFile file(envImagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray data = file.read(kMaxEnvBytes);
    for (const qsizetype headerBytes : {qsizetype(4), qsizetype(5)}) {
        qsizetype pos = headerBytes;
        while (pos < data.size()) {
            const qsizetype nul = data.indexOf('\0', pos);
            const qsizetype end = nul < 0 ? data.size() : nul;
            if (end == pos) {
                break;   // an empty entry: the end of the environment
            }
            if (end - pos > key.size() && data.mid(pos, key.size()) == key) {
                return QString::fromLatin1(data.mid(pos + key.size(), end - pos - key.size())).trimmed();
            }
            pos = end + 1;
        }
    }
    return QString();
}

QList<QPair<QString, QString>> RockchipTool::parsePackageFile(const QString& text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s"));
    QList<QPair<QString, QString>> entries;
    for (const QString& rawLine : splitLines(text)) {
        QString line = rawLine.trimmed();
        if (line.startsWith(QChar(0xFEFF))) {
            line = line.mid(1).trimmed();   // a byte-order mark in front of the first line
        }
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const qsizetype gap = line.indexOf(whitespace);
        if (gap < 0) {
            continue;   // a partition name without a file
        }
        const QString file = line.mid(gap).trimmed();
        if (!file.isEmpty()) {
            entries.append({line.left(gap), file});
        }
    }
    return entries;
}

QList<PartitionImage> RockchipTool::planPartitions(const QString& extractDir, const QList<MtdPartition>& layout,
                                                   QString* error)
{
    if (error) {
        error->clear();
    }
    const QDir dir(extractDir);
    QFile packageFile(dir.filePath(QStringLiteral("package-file")));
    if (!packageFile.exists()) {
        if (error) {
            *error = QCoreApplication::translate("RockchipTool", "no package-file in %1: the image was not unpacked")
                         .arg(QDir::toNativeSeparators(extractDir));
        }
        return {};
    }
    if (!packageFile.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QCoreApplication::translate("RockchipTool", "cannot read %1: %2")
                         .arg(QDir::toNativeSeparators(packageFile.fileName()), packageFile.errorString());
        }
        return {};
    }
    QList<PartitionImage> rows;
    for (const auto& [name, fileName] : parsePackageFile(QString::fromUtf8(packageFile.readAll()))) {
        // upgrade_tool's own entries: the list itself and the loader (download.bin), which the
        // board's loader in Loader mode does not need and no partition holds.
        if (name.compare(QLatin1String("package-file"), Qt::CaseInsensitive) == 0
            || name.compare(QLatin1String("bootloader"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        PartitionImage row;
        row.name = name;
        row.file = QDir::cleanPath(dir.absoluteFilePath(fileName));
        const QFileInfo info(row.file);
        row.fileSize = info.isFile() ? info.size() : -1;
        for (const MtdPartition& partition : layout) {
            if (partition.name == name) {
                row.inLayout = true;
                row.offset = partition.offset;
                row.size = partition.size;
                break;
            }
        }
        // A missing file (a "RESERVED" entry, an extraction that stopped) fits nothing: there is
        // nothing to write.
        row.fits = row.fileSize >= 0 && (row.size == 0 || static_cast<quint64>(row.fileSize) <= row.size);
        row.selected = row.inLayout && row.fits;
        rows.append(row);
    }
    return rows;
}

// ---------------------------------------------------------------------------------------------
// ProgressParser
// ---------------------------------------------------------------------------------------------

namespace {

/// One finished UF segment -> stage text and percentage; nothing for a blank segment.
bool parseSegment(const QByteArray& raw, const QString& previousStage, RockchipTool::Progress* out)
{
    const QString text = QString::fromUtf8(raw).trimmed();
    if (text.isEmpty()) {
        return false;
    }
    static const QRegularExpression percent(QStringLiteral("\\(\\s*(\\d{1,3})\\s*%\\s*\\)"));
    int value = -1;
    qsizetype cut = text.size();
    QRegularExpressionMatchIterator it = percent.globalMatch(text);
    QRegularExpressionMatch last;
    while (it.hasNext()) {
        last = it.next();
    }
    if (last.hasMatch()) {
        value = std::clamp(last.captured(1).toInt(), 0, 100);
        cut = last.capturedStart(0);
    }
    QString stage = text.left(cut).trimmed();
    if (stage.isEmpty() && value >= 0) {
        stage = previousStage;   // a bare "(42%)" continues the running stage
    }
    out->stage = stage;
    out->percent = value;
    return true;
}

} // namespace

QList<RockchipTool::Progress> RockchipTool::ProgressParser::feed(const QByteArray& chunk)
{
    QList<Progress> reported;
    m_tail.append(chunk);
    qsizetype start = 0;
    for (qsizetype i = 0; i < m_tail.size(); ++i) {
        const char c = m_tail.at(i);
        if (c != '\r' && c != '\n') {
            continue;
        }
        Progress progress;
        if (parseSegment(m_tail.mid(start, i - start), m_lastStage, &progress)) {
            reported.append(progress);
            m_lastStage = progress.stage;
            m_lastPercent = progress.percent;
        }
        start = i + 1;
    }
    m_tail.remove(0, start);   // the unfinished segment waits for its terminator
    return reported;
}

QList<RockchipTool::Progress> RockchipTool::ProgressParser::flush()
{
    QList<Progress> reported;
    Progress progress;
    if (parseSegment(m_tail, m_lastStage, &progress)) {
        reported.append(progress);
        m_lastStage = progress.stage;
        m_lastPercent = progress.percent;
    }
    m_tail.clear();
    return reported;
}
