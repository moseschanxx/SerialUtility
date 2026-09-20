#include "core/QuickCommand.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QCoreApplication>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/HexUtils.h"

namespace {

constexpr int kFileVersion = 1;

QuickCommand makeText(const QString& group, const QString& name, const QString& command,
                      LineEnding::Mode ending = LineEnding::Mode::CR)
{
    QuickCommand qc;
    qc.group = group;
    qc.name = name;
    qc.command = command;
    qc.lineEnding = ending;
    return qc;
}

QuickCommand makeHex(const QString& group, const QString& name, const QString& hexBytes, const QString& tooltip)
{
    QuickCommand qc;
    qc.group = group;
    qc.name = name;
    qc.command = hexBytes;
    qc.hex = true;
    qc.lineEnding = LineEnding::Mode::None;
    qc.tooltip = tooltip;
    return qc;
}

/// Parse a { "version": 1, "commands": [...] } document. Returns false with *error on any
/// structural problem; a "commands" entry that is not an object is skipped.
bool parseDocument(const QByteArray& json, QList<QuickCommand>& out, QString* error)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = QCoreApplication::translate("QuickCommandStore", "Invalid JSON at offset %1: %2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        }
        return false;
    }
    if (!doc.isObject()) {
        if (error) {
            *error = QCoreApplication::translate("QuickCommandStore", "Top-level JSON value is not an object");
        }
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonValue commands = root.value(QLatin1String("commands"));
    if (!commands.isArray()) {
        if (error) {
            *error = QCoreApplication::translate("QuickCommandStore", "Missing \"commands\" array");
        }
        return false;
    }
    out.clear();
    const QJsonArray array = commands.toArray();
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        out.append(QuickCommand::fromJson(value.toObject()));
    }
    return true;
}

QByteArray serialize(const QList<QuickCommand>& commands)
{
    QJsonArray array;
    for (const QuickCommand& qc : commands) {
        array.append(qc.toJson());
    }
    QJsonObject root;
    root.insert(QLatin1String("version"), kFileVersion);
    root.insert(QLatin1String("commands"), array);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeFileAtomically(const QString& path, const QByteArray& bytes, QString* error)
{
    const QFileInfo info(path);
    const QDir dir = info.dir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (error) {
            *error = QCoreApplication::translate("QuickCommandStore", "Cannot create directory %1")
                         .arg(QDir::toNativeSeparators(dir.absolutePath()));
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QCoreApplication::translate("QuickCommandStore", "Cannot write %1: %2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        }
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) {
            *error = QCoreApplication::translate("QuickCommandStore", "Cannot write %1: %2")
                         .arg(QDir::toNativeSeparators(path), file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------
// QuickCommand
// ---------------------------------------------------------------------------------------

QByteArray QuickCommand::payload(QString* error) const
{
    if (error) {
        error->clear();
    }
    if (hex) {
        QByteArray bytes;
        if (!HexUtils::parseHexString(command, bytes, error)) {
            return QByteArray();
        }
        return bytes;
    }
    QByteArray bytes;
    if (escapes) {
        QString unescapeError;
        bytes = HexUtils::unescape(command, &unescapeError);
        if (!unescapeError.isEmpty()) {
            if (error) {
                *error = unescapeError;
            }
            return QByteArray();
        }
    } else {
        bytes = command.toUtf8();
    }
    bytes.append(LineEnding::bytes(lineEnding));
    return bytes;
}

QJsonObject QuickCommand::toJson() const
{
    QJsonObject obj;
    obj.insert(QLatin1String("name"), name);
    obj.insert(QLatin1String("command"), command);
    obj.insert(QLatin1String("group"), group);
    obj.insert(QLatin1String("lineEnding"), LineEnding::toString(lineEnding));
    obj.insert(QLatin1String("hex"), hex);
    obj.insert(QLatin1String("escapes"), escapes);
    obj.insert(QLatin1String("shortcut"), shortcut);
    obj.insert(QLatin1String("tooltip"), tooltip);
    return obj;
}

QuickCommand QuickCommand::fromJson(const QJsonObject& obj)
{
    QuickCommand qc;
    qc.name = obj.value(QLatin1String("name")).toString();
    qc.command = obj.value(QLatin1String("command")).toString();
    qc.group = obj.value(QLatin1String("group")).toString();
    const QJsonValue ending = obj.value(QLatin1String("lineEnding"));
    qc.lineEnding = ending.isString() ? LineEnding::fromString(ending.toString()) : LineEnding::Mode::CR;
    qc.hex = obj.value(QLatin1String("hex")).toBool(false);
    qc.escapes = obj.value(QLatin1String("escapes")).toBool(false);
    qc.shortcut = obj.value(QLatin1String("shortcut")).toString();
    qc.tooltip = obj.value(QLatin1String("tooltip")).toString();
    return qc;
}

bool QuickCommand::operator==(const QuickCommand& other) const
{
    return name == other.name && command == other.command && group == other.group &&
           lineEnding == other.lineEnding && hex == other.hex && escapes == other.escapes &&
           shortcut == other.shortcut && tooltip == other.tooltip;
}

// ---------------------------------------------------------------------------------------
// QuickCommandStore
// ---------------------------------------------------------------------------------------

QuickCommandStore::QuickCommandStore(QObject* parent)
    : QObject(parent)
    , m_commands(defaults())
{
}

QString QuickCommandStore::defaultFilePath()
{
    return AppSettings::dataDirectory() + QStringLiteral("/quick_commands.json");
}

bool QuickCommandStore::load(const QString& path)
{
    const QString filePath = path.isEmpty() ? defaultFilePath() : path;
    QFile file(filePath);
    if (!file.exists()) {
        qCInfo(lcApp) << "quick commands file" << QDir::toNativeSeparators(filePath)
                      << "does not exist, using defaults";
        setCommands(defaults());
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcApp) << "cannot read quick commands" << QDir::toNativeSeparators(filePath) << file.errorString();
        return false;
    }
    QList<QuickCommand> loaded;
    QString error;
    if (!parseDocument(file.readAll(), loaded, &error)) {
        qCWarning(lcApp) << "malformed quick commands file" << QDir::toNativeSeparators(filePath) << error;
        return false;
    }
    qCInfo(lcApp) << "loaded" << loaded.size() << "quick commands from" << QDir::toNativeSeparators(filePath);
    setCommands(loaded);
    return true;
}

bool QuickCommandStore::save(const QString& path) const
{
    const QString filePath = path.isEmpty() ? defaultFilePath() : path;
    QString error;
    if (!writeFileAtomically(filePath, serialize(m_commands), &error)) {
        qCWarning(lcApp) << "cannot save quick commands:" << error;
        return false;
    }
    qCDebug(lcApp) << "saved" << m_commands.size() << "quick commands to" << QDir::toNativeSeparators(filePath);
    return true;
}

QList<QuickCommand> QuickCommandStore::commands() const
{
    return m_commands;
}

void QuickCommandStore::setCommands(const QList<QuickCommand>& commands)
{
    if (m_commands == commands) {
        return;
    }
    m_commands = commands;
    emit changed();
}

QStringList QuickCommandStore::groups() const
{
    QStringList result;
    for (const QuickCommand& qc : m_commands) {
        const QString group = qc.group.isEmpty() ? tr("General") : qc.group;
        if (!result.contains(group)) {
            result.append(group);
        }
    }
    return result;
}

QList<QuickCommand> QuickCommandStore::defaults()
{
    const QString linux = QStringLiteral("Linux");
    const QString uboot = QStringLiteral("U-Boot");
    const QString mcu = QStringLiteral("MCU");
    const QString control = QStringLiteral("Control");

    QList<QuickCommand> list;
    // Linux
    list.append(makeText(linux, QStringLiteral("uname -a"), QStringLiteral("uname -a")));
    list.append(makeText(linux, QStringLiteral("cpuinfo"), QStringLiteral("cat /proc/cpuinfo")));
    list.append(makeText(linux, QStringLiteral("meminfo"), QStringLiteral("cat /proc/meminfo")));
    list.append(makeText(linux, QStringLiteral("df -h"), QStringLiteral("df -h")));
    list.append(makeText(linux, QStringLiteral("ifconfig"), QStringLiteral("ifconfig")));
    list.append(makeText(linux, QStringLiteral("dmesg tail"), QStringLiteral("dmesg | tail -n 50")));
    list.append(makeText(linux, QStringLiteral("ps"), QStringLiteral("ps")));
    list.append(makeText(linux, QStringLiteral("top"), QStringLiteral("top -n 1")));
    list.append(makeText(linux, QStringLiteral("ls /dev"), QStringLiteral("ls -l /dev/tty* /dev/video* 2>/dev/null")));
    list.append(makeText(linux, QStringLiteral("date"), QStringLiteral("date")));
    list.append(makeText(linux, QStringLiteral("reboot"), QStringLiteral("reboot")));
    // U-Boot
    list.append(makeText(uboot, QStringLiteral("help"), QStringLiteral("help")));
    list.append(makeText(uboot, QStringLiteral("printenv"), QStringLiteral("printenv")));
    list.append(makeText(uboot, QStringLiteral("bdinfo"), QStringLiteral("bdinfo")));
    list.append(makeText(uboot, QStringLiteral("version"), QStringLiteral("version")));
    list.append(makeText(uboot, QStringLiteral("mmc info"), QStringLiteral("mmc info")));
    list.append(makeText(uboot, QStringLiteral("boot"), QStringLiteral("boot")));
    list.append(makeText(uboot, QStringLiteral("reset"), QStringLiteral("reset")));
    // MCU
    list.append(makeText(mcu, QStringLiteral("help"), QStringLiteral("help")));
    list.append(makeText(mcu, QStringLiteral("version"), QStringLiteral("version")));
    list.append(makeText(mcu, QStringLiteral("AT"), QStringLiteral("AT"), LineEnding::Mode::CRLF));
    list.append(makeText(mcu, QStringLiteral("AT+GMR"), QStringLiteral("AT+GMR"), LineEnding::Mode::CRLF));
    list.append(makeText(mcu, QStringLiteral("reset"), QStringLiteral("reset")));
    // Control bytes
    list.append(makeHex(control, QStringLiteral("Ctrl+C"), QStringLiteral("03"), tr("Interrupt (ETX, 0x03)")));
    list.append(
        makeHex(control, QStringLiteral("Ctrl+D"), QStringLiteral("04"), tr("End of transmission (EOT, 0x04)")));
    list.append(makeHex(control, QStringLiteral("Ctrl+Z"), QStringLiteral("1A"), tr("Suspend (SUB, 0x1A)")));
    list.append(makeHex(control, QStringLiteral("ESC"), QStringLiteral("1B"), tr("Escape (0x1B)")));
    list.append(makeHex(control, QStringLiteral("Enter"), QStringLiteral("0D"), tr("Carriage return (0x0D)")));
    list.append(
        makeHex(control, QStringLiteral("Ctrl+L"), QStringLiteral("0C"), tr("Form feed / clear screen (0x0C)")));
    return list;
}

void QuickCommandStore::resetToDefaults()
{
    setCommands(defaults());
}

bool QuickCommandStore::importFromFile(const QString& path, QString* error)
{
    if (error) {
        error->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        }
        return false;
    }
    QList<QuickCommand> imported;
    if (!parseDocument(file.readAll(), imported, error)) {
        return false;
    }
    qCInfo(lcApp) << "imported" << imported.size() << "quick commands from" << QDir::toNativeSeparators(path);
    setCommands(imported);
    return true;
}

bool QuickCommandStore::exportToFile(const QString& path, QString* error) const
{
    if (error) {
        error->clear();
    }
    if (!writeFileAtomically(path, serialize(m_commands), error)) {
        return false;
    }
    qCInfo(lcApp) << "exported" << m_commands.size() << "quick commands to" << QDir::toNativeSeparators(path);
    return true;
}
