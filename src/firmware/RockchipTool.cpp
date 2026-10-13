#include "firmware/RockchipTool.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>

#include <algorithm>

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
