#include "app/AppSettings.h"

#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QLocale>
#include <QVariant>

#include "app/Logging.h"

namespace {

// Key names (kept in one place for reference / migration).
constexpr auto kLanguage = "general/language";
constexpr auto kTerminalFont = "terminal/font";
constexpr auto kTheme = "terminal/theme";
constexpr auto kScrollback = "terminal/scrollback";
constexpr auto kCursorBlink = "terminal/cursorBlink";
constexpr auto kBell = "terminal/bell";
constexpr auto kImplicitCr = "terminal/implicitCr";
constexpr auto kPauseWhileSelecting = "terminal/pauseWhileSelecting";
constexpr auto kRightClickPastes = "terminal/rightClickPastes";
constexpr auto kEnterSends = "input/enterSends";
constexpr auto kBackspaceDelete = "input/backspaceSendsDelete";
constexpr auto kLocalEcho = "input/localEcho";
constexpr auto kEncoding = "input/encoding";
constexpr auto kConnectionDefault = "connection/default";
constexpr auto kAutoReconnect = "connection/autoReconnect";
constexpr auto kReconnectInterval = "connection/reconnectIntervalMs";
constexpr auto kLastPort = "connection/lastPort";
constexpr auto kShowSimulatedPorts = "connection/showSimulatedPorts";
constexpr auto kSshKnownHostsFile = "ssh/knownHostsFile";
constexpr auto kSshTerminalType = "ssh/terminalType";
constexpr auto kSshKeepAliveSeconds = "ssh/keepAliveSeconds";
constexpr auto kSshIdentityFile = "ssh/identityFile";
constexpr auto kSshLastTarget = "ssh/lastTarget";
constexpr auto kLogDirectory = "logging/directory";
constexpr auto kAutoLog = "logging/autoLog";
constexpr auto kLogFormat = "logging/format";
constexpr auto kLogIncludeTx = "logging/includeTx";
constexpr auto kConfirmClose = "session/confirmClose";
constexpr auto kRestoreLastPorts = "session/restoreLastPorts";
constexpr auto kLastOpenPorts = "session/lastOpenPorts";
constexpr auto kWindowGeometry = "window/geometry";
constexpr auto kWindowState = "window/state";
constexpr auto kShortcutsGroup = "shortcuts";
constexpr auto kAutoBaudCandidates = "connection/autoBaudCandidates";
constexpr auto kAutoBaudSampleMs = "connection/autoBaudSampleMs";
constexpr auto kAutoBaudWatchdog = "connection/autoBaudWatchdog";

constexpr int kMinScrollback = 100;
constexpr int kMaxScrollback = 1000000;
constexpr int kDefaultScrollback = 10000;
constexpr int kMinReconnectMs = 200;
constexpr int kMaxReconnectMs = 60000;
constexpr int kDefaultReconnectMs = 1000;
constexpr int kMinSshKeepAlive = 0;
constexpr int kMaxSshKeepAlive = 600;
constexpr int kDefaultSshKeepAlive = 30;
const QLatin1String kDefaultSshTerminalType("xterm-256color");
constexpr int kMinAutoBaudSampleMs = 300;
constexpr int kMaxAutoBaudSampleMs = 10000;
constexpr int kDefaultAutoBaudSampleMs = 1500;

QVariant readValue(const char* key, const QVariant& fallback = QVariant())
{
    QSettings settings;
    return settings.value(QLatin1String(key), fallback);
}

QString defaultLanguage()
{
    const QLocale locale = QLocale::system();
    if (locale.language() == QLocale::Chinese) {
        return QStringLiteral("zh_CN");
    }
    return QStringLiteral("en_US");
}

QString normalizeLogFormat(const QString& format)
{
    const QString f = format.trimmed().toLower();
    if (f == QLatin1String("raw") || f == QLatin1String("hex")) {
        return f;
    }
    return QStringLiteral("text");
}

} // namespace

AppSettings& AppSettings::instance()
{
    static AppSettings s_instance;
    return s_instance;
}

AppSettings::AppSettings(QObject* parent)
    : QObject(parent)
{
}

QFont AppSettings::defaultTerminalFont()
{
#if defined(Q_OS_WIN)
    QFont font(QStringLiteral("Consolas"), 10);
#else
    QFont font(QStringLiteral("Monospace"), 10);
#endif
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QString AppSettings::defaultLogDirectory()
{
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (docs.isEmpty()) {
        docs = QDir::homePath();
    }
    return docs + QStringLiteral("/BuildAI/SerialLogs");
}

// Writes `value` under `key` and emits changed(key).
#define SU_WRITE_SETTING(key, value)                                                                                  \
    do {                                                                                                              \
        QSettings su_qsettings;                                                                                       \
        su_qsettings.setValue(QLatin1String(key), (value));                                                           \
        emit changed(QLatin1String(key));                                                                             \
    } while (false)

// ---- General ---------------------------------------------------------------------------

QString AppSettings::language() const
{
    const QString code = readValue(kLanguage).toString().trimmed();
    return code.isEmpty() ? defaultLanguage() : code;
}

void AppSettings::setLanguage(const QString& code)
{
    SU_WRITE_SETTING(kLanguage, code);
}

// ---- Terminal appearance ---------------------------------------------------------------

QFont AppSettings::terminalFont() const
{
    const QString spec = readValue(kTerminalFont).toString();
    QFont font = AppSettings::defaultTerminalFont();
    if (!spec.isEmpty()) {
        QFont stored;
        if (stored.fromString(spec)) {
            font = stored;
        } else {
            qCWarning(lcApp) << "invalid font specification in settings:" << spec;
        }
    }
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

void AppSettings::setTerminalFont(const QFont& font)
{
    SU_WRITE_SETTING(kTerminalFont, font.toString());
}

QString AppSettings::themeName() const
{
    const QString name = readValue(kTheme).toString().trimmed();
    return name.isEmpty() ? QStringLiteral("dark") : name;
}

void AppSettings::setThemeName(const QString& name)
{
    SU_WRITE_SETTING(kTheme, name);
}

int AppSettings::scrollbackLines() const
{
    bool ok = false;
    const int lines = readValue(kScrollback, kDefaultScrollback).toInt(&ok);
    return qBound(kMinScrollback, ok ? lines : kDefaultScrollback, kMaxScrollback);
}

void AppSettings::setScrollbackLines(int lines)
{
    SU_WRITE_SETTING(kScrollback, qBound(kMinScrollback, lines, kMaxScrollback));
}

bool AppSettings::cursorBlink() const
{
    return readValue(kCursorBlink, true).toBool();
}

void AppSettings::setCursorBlink(bool on)
{
    SU_WRITE_SETTING(kCursorBlink, on);
}

bool AppSettings::bellEnabled() const
{
    return readValue(kBell, true).toBool();
}

void AppSettings::setBellEnabled(bool on)
{
    SU_WRITE_SETTING(kBell, on);
}

bool AppSettings::implicitCr() const
{
    return readValue(kImplicitCr, true).toBool();
}

void AppSettings::setImplicitCr(bool on)
{
    SU_WRITE_SETTING(kImplicitCr, on);
}

bool AppSettings::pauseWhileSelecting() const
{
    return readValue(kPauseWhileSelecting, true).toBool();
}

void AppSettings::setPauseWhileSelecting(bool on)
{
    SU_WRITE_SETTING(kPauseWhileSelecting, on);
}

bool AppSettings::rightClickPastes() const
{
    return readValue(kRightClickPastes, true).toBool();
}

void AppSettings::setRightClickPastes(bool on)
{
    SU_WRITE_SETTING(kRightClickPastes, on);
}

// ---- Input behaviour -------------------------------------------------------------------

LineEnding::Mode AppSettings::enterSends() const
{
    return LineEnding::fromString(readValue(kEnterSends, LineEnding::toString(LineEnding::Mode::CR)).toString());
}

void AppSettings::setEnterSends(LineEnding::Mode mode)
{
    SU_WRITE_SETTING(kEnterSends, LineEnding::toString(mode));
}

bool AppSettings::backspaceSendsDelete() const
{
    return readValue(kBackspaceDelete, true).toBool();
}

void AppSettings::setBackspaceSendsDelete(bool on)
{
    SU_WRITE_SETTING(kBackspaceDelete, on);
}

bool AppSettings::localEcho() const
{
    return readValue(kLocalEcho, false).toBool();
}

void AppSettings::setLocalEcho(bool on)
{
    SU_WRITE_SETTING(kLocalEcho, on);
}

QString AppSettings::encoding() const
{
    const QString name = readValue(kEncoding).toString().trimmed();
    return name.isEmpty() ? QStringLiteral("UTF-8") : name;
}

void AppSettings::setEncoding(const QString& name)
{
    SU_WRITE_SETTING(kEncoding, name);
}

// ---- Connection ------------------------------------------------------------------------

SerialSettings AppSettings::defaultSerialSettings() const
{
    const QVariant value = readValue(kConnectionDefault);
    if (!value.isValid() || !value.canConvert<QVariantMap>()) {
        return SerialSettings();
    }
    return SerialSettings::fromMap(value.toMap());
}

void AppSettings::setDefaultSerialSettings(const SerialSettings& settings)
{
    SU_WRITE_SETTING(kConnectionDefault, settings.toMap());
}

bool AppSettings::autoReconnect() const
{
    return readValue(kAutoReconnect, true).toBool();
}

void AppSettings::setAutoReconnect(bool on)
{
    SU_WRITE_SETTING(kAutoReconnect, on);
}

int AppSettings::reconnectIntervalMs() const
{
    bool ok = false;
    const int ms = readValue(kReconnectInterval, kDefaultReconnectMs).toInt(&ok);
    return qBound(kMinReconnectMs, ok ? ms : kDefaultReconnectMs, kMaxReconnectMs);
}

void AppSettings::setReconnectIntervalMs(int ms)
{
    SU_WRITE_SETTING(kReconnectInterval, qBound(kMinReconnectMs, ms, kMaxReconnectMs));
}

QString AppSettings::lastPortName() const
{
    return readValue(kLastPort).toString();
}

void AppSettings::setLastPortName(const QString& name)
{
    SU_WRITE_SETTING(kLastPort, name);
}

bool AppSettings::showSimulatedPorts() const
{
    return readValue(kShowSimulatedPorts, true).toBool();
}

void AppSettings::setShowSimulatedPorts(bool on)
{
    SU_WRITE_SETTING(kShowSimulatedPorts, on);
}

// ---- SSH -------------------------------------------------------------------------------

QString AppSettings::sshKnownHostsFile() const
{
    return readValue(kSshKnownHostsFile).toString().trimmed();
}

void AppSettings::setSshKnownHostsFile(const QString& path)
{
    SU_WRITE_SETTING(kSshKnownHostsFile, path.trimmed());
}

QString AppSettings::sshDefaultTerminalType() const
{
    const QString type = readValue(kSshTerminalType).toString().trimmed();
    return type.isEmpty() ? QString(kDefaultSshTerminalType) : type;
}

void AppSettings::setSshDefaultTerminalType(const QString& type)
{
    const QString trimmed = type.trimmed();
    SU_WRITE_SETTING(kSshTerminalType, trimmed.isEmpty() ? QString(kDefaultSshTerminalType) : trimmed);
}

int AppSettings::sshDefaultKeepAliveSeconds() const
{
    bool ok = false;
    const int seconds = readValue(kSshKeepAliveSeconds, kDefaultSshKeepAlive).toInt(&ok);
    return qBound(kMinSshKeepAlive, ok ? seconds : kDefaultSshKeepAlive, kMaxSshKeepAlive);
}

void AppSettings::setSshDefaultKeepAliveSeconds(int seconds)
{
    SU_WRITE_SETTING(kSshKeepAliveSeconds, qBound(kMinSshKeepAlive, seconds, kMaxSshKeepAlive));
}

QString AppSettings::sshDefaultIdentityFile() const
{
    return readValue(kSshIdentityFile).toString().trimmed();
}

void AppSettings::setSshDefaultIdentityFile(const QString& path)
{
    SU_WRITE_SETTING(kSshIdentityFile, path.trimmed());
}

QString AppSettings::lastSshTarget() const
{
    return readValue(kSshLastTarget).toString();
}

void AppSettings::setLastSshTarget(const QString& key)
{
    SU_WRITE_SETTING(kSshLastTarget, key);
}

// ---- Logging ---------------------------------------------------------------------------

QString AppSettings::logDirectory() const
{
    const QString dir = readValue(kLogDirectory).toString().trimmed();
    return dir.isEmpty() ? AppSettings::defaultLogDirectory() : dir;
}

void AppSettings::setLogDirectory(const QString& dir)
{
    SU_WRITE_SETTING(kLogDirectory, dir);
}

bool AppSettings::autoLog() const
{
    return readValue(kAutoLog, false).toBool();
}

void AppSettings::setAutoLog(bool on)
{
    SU_WRITE_SETTING(kAutoLog, on);
}

QString AppSettings::logFormat() const
{
    return normalizeLogFormat(readValue(kLogFormat, QStringLiteral("text")).toString());
}

void AppSettings::setLogFormat(const QString& format)
{
    SU_WRITE_SETTING(kLogFormat, normalizeLogFormat(format));
}

bool AppSettings::logIncludeTx() const
{
    return readValue(kLogIncludeTx, true).toBool();
}

void AppSettings::setLogIncludeTx(bool on)
{
    SU_WRITE_SETTING(kLogIncludeTx, on);
}

// ---- Session / window ------------------------------------------------------------------

bool AppSettings::confirmCloseWhenConnected() const
{
    return readValue(kConfirmClose, true).toBool();
}

void AppSettings::setConfirmCloseWhenConnected(bool on)
{
    SU_WRITE_SETTING(kConfirmClose, on);
}

bool AppSettings::restoreLastPorts() const
{
    return readValue(kRestoreLastPorts, true).toBool();
}

void AppSettings::setRestoreLastPorts(bool on)
{
    SU_WRITE_SETTING(kRestoreLastPorts, on);
}

QStringList AppSettings::lastOpenPorts() const
{
    return readValue(kLastOpenPorts).toStringList();
}

void AppSettings::setLastOpenPorts(const QStringList& ports)
{
    SU_WRITE_SETTING(kLastOpenPorts, ports);
}

QByteArray AppSettings::mainWindowGeometry() const
{
    return readValue(kWindowGeometry).toByteArray();
}

void AppSettings::setMainWindowGeometry(const QByteArray& geometry)
{
    SU_WRITE_SETTING(kWindowGeometry, geometry);
}

QByteArray AppSettings::mainWindowState() const
{
    return readValue(kWindowState).toByteArray();
}

void AppSettings::setMainWindowState(const QByteArray& state)
{
    SU_WRITE_SETTING(kWindowState, state);
}

QString AppSettings::dataDirectory()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.BuildAI/SerialUtility");
    }
    if (!QDir(dir).exists() && !QDir().mkpath(dir)) {
        qCWarning(lcApp) << "cannot create data directory" << QDir::toNativeSeparators(dir);
    }
    return dir;
}

void AppSettings::sync()
{
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qCWarning(lcApp) << "settings sync failed with status" << static_cast<int>(settings.status());
    }
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts
// ---------------------------------------------------------------------------

QKeySequence AppSettings::shortcut(const QString& actionName, const QKeySequence& fallback) const
{
    QSettings settings;
    const QString key = QLatin1String(kShortcutsGroup) + QLatin1Char('/') + actionName;
    if (!settings.contains(key)) {
        return fallback;
    }
    return QKeySequence::fromString(settings.value(key).toString(), QKeySequence::PortableText);
}

void AppSettings::setShortcut(const QString& actionName, const QKeySequence& sequence)
{
    if (actionName.trimmed().isEmpty()) {
        return;
    }
    const QString key = QLatin1String(kShortcutsGroup) + QLatin1Char('/') + actionName;
    {
        QSettings settings;
        settings.setValue(key, sequence.toString(QKeySequence::PortableText));
    }
    emit changed(key);
}

void AppSettings::clearShortcut(const QString& actionName)
{
    const QString key = QLatin1String(kShortcutsGroup) + QLatin1Char('/') + actionName;
    {
        QSettings settings;
        if (!settings.contains(key)) {
            return;
        }
        settings.remove(key);
    }
    emit changed(key);
}

QStringList AppSettings::customizedShortcutActions() const
{
    QSettings settings;
    settings.beginGroup(QLatin1String(kShortcutsGroup));
    QStringList names = settings.childKeys();
    settings.endGroup();
    names.sort();
    return names;
}

// ---------------------------------------------------------------------------
// Automatic baud-rate detection
// ---------------------------------------------------------------------------

QList<qint32> AppSettings::defaultAutoBaudCandidates()
{
    return {115200, 1500000, 921600, 460800, 230400, 57600, 38400, 19200, 9600};
}

QList<qint32> AppSettings::autoBaudCandidates() const
{
    QList<qint32> result;
    const QStringList parts = readValue(kAutoBaudCandidates).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const qint32 baud = part.trimmed().toInt(&ok);
        if (ok && SerialSettings::isValidBaudRate(baud) && !result.contains(baud)) {
            result.append(baud);
        }
    }
    return result.isEmpty() ? defaultAutoBaudCandidates() : result;
}

void AppSettings::setAutoBaudCandidates(const QList<qint32>& candidates)
{
    QStringList parts;
    for (qint32 baud : candidates) {
        if (SerialSettings::isValidBaudRate(baud) && !parts.contains(QString::number(baud))) {
            parts.append(QString::number(baud));
        }
    }
    SU_WRITE_SETTING(kAutoBaudCandidates, parts.join(QLatin1Char(',')));
}

int AppSettings::autoBaudSampleMs() const
{
    bool ok = false;
    const int ms = readValue(kAutoBaudSampleMs, kDefaultAutoBaudSampleMs).toInt(&ok);
    return qBound(kMinAutoBaudSampleMs, ok ? ms : kDefaultAutoBaudSampleMs, kMaxAutoBaudSampleMs);
}

void AppSettings::setAutoBaudSampleMs(int ms)
{
    SU_WRITE_SETTING(kAutoBaudSampleMs, qBound(kMinAutoBaudSampleMs, ms, kMaxAutoBaudSampleMs));
}

bool AppSettings::autoBaudWatchdog() const
{
    return readValue(kAutoBaudWatchdog, true).toBool();
}

void AppSettings::setAutoBaudWatchdog(bool on)
{
    SU_WRITE_SETTING(kAutoBaudWatchdog, on);
}

#undef SU_WRITE_SETTING
