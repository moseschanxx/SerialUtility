#include "ssh/SshProfile.h"

#include "app/AppSettings.h"
#include "app/Logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace {

constexpr int kStoreVersion = 1;
constexpr int kMaxRecentTargets = 10;

bool lessByName(const SshProfile& a, const SshProfile& b)
{
    const int cmp = QString::compare(a.displayName(), b.displayName(), Qt::CaseInsensitive);
    if (cmp != 0) {
        return cmp < 0;
    }
    return a.id < b.id;
}

QString newProfileId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString bracketHost(const QString& host)
{
    // IPv6 literals are written in brackets when a port or user is attached ("[::1]:2222").
    return host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(host) : host;
}

} // namespace

// ---------------------------------------------------------------------------
// SshLocalForward
// ---------------------------------------------------------------------------

QJsonObject SshLocalForward::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("localPort"), static_cast<int>(localPort));
    obj.insert(QStringLiteral("remoteHost"), remoteHost);
    obj.insert(QStringLiteral("remotePort"), static_cast<int>(remotePort));
    obj.insert(QStringLiteral("bindAddress"), bindAddress);
    return obj;
}

SshLocalForward SshLocalForward::fromJson(const QJsonObject& obj)
{
    SshLocalForward fwd;
    const int local = obj.value(QStringLiteral("localPort")).toInt(0);
    const int remote = obj.value(QStringLiteral("remotePort")).toInt(0);
    fwd.localPort = (local > 0 && local <= 65535) ? static_cast<quint16>(local) : 0;
    fwd.remotePort = (remote > 0 && remote <= 65535) ? static_cast<quint16>(remote) : 0;
    fwd.remoteHost = obj.value(QStringLiteral("remoteHost")).toString(fwd.remoteHost);
    fwd.bindAddress = obj.value(QStringLiteral("bindAddress")).toString(fwd.bindAddress);
    if (fwd.bindAddress.trimmed().isEmpty()) {
        fwd.bindAddress = QStringLiteral("127.0.0.1");
    }
    return fwd;
}

QString SshLocalForward::displayText() const
{
    const QString local = (bindAddress.isEmpty() || bindAddress == QLatin1String("127.0.0.1")
                           || bindAddress == QLatin1String("localhost"))
        ? QString::number(localPort)
        : QStringLiteral("%1:%2").arg(bracketHost(bindAddress)).arg(localPort);
    return QStringLiteral("%1 -> %2:%3").arg(local, bracketHost(remoteHost)).arg(remotePort);
}

bool SshLocalForward::operator==(const SshLocalForward& other) const
{
    return localPort == other.localPort && remoteHost == other.remoteHost && remotePort == other.remotePort
        && bindAddress == other.bindAddress;
}

// ---------------------------------------------------------------------------
// SshProfile
// ---------------------------------------------------------------------------

QJsonObject SshProfile::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("host"), host);
    obj.insert(QStringLiteral("port"), static_cast<int>(port));
    obj.insert(QStringLiteral("user"), user);
    obj.insert(QStringLiteral("auth"), authKey(auth));
    obj.insert(QStringLiteral("identityFile"), identityFile);
    obj.insert(QStringLiteral("passwordSaved"), passwordSaved);
    obj.insert(QStringLiteral("remoteCommand"), remoteCommand);
    obj.insert(QStringLiteral("startupCommand"), startupCommand);
    obj.insert(QStringLiteral("terminalType"), terminalType);
    obj.insert(QStringLiteral("keepAliveSeconds"), keepAliveSeconds);
    obj.insert(QStringLiteral("connectTimeoutSeconds"), connectTimeoutSeconds);
    obj.insert(QStringLiteral("proxyJump"), proxyJump);
    QJsonArray forwards;
    for (const SshLocalForward& fwd : localForwards) {
        forwards.append(fwd.toJson());
    }
    obj.insert(QStringLiteral("localForwards"), forwards);
    obj.insert(QStringLiteral("compression"), compression);
    obj.insert(QStringLiteral("knownHostsFile"), knownHostsFile);
    obj.insert(QStringLiteral("description"), description);
    if (lastUsed.isValid()) {
        obj.insert(QStringLiteral("lastUsed"), lastUsed.toUTC().toString(Qt::ISODateWithMs));
    }
    return obj;
}

SshProfile SshProfile::fromJson(const QJsonObject& obj)
{
    SshProfile p;
    p.id = obj.value(QStringLiteral("id")).toString();
    p.name = obj.value(QStringLiteral("name")).toString();
    p.host = obj.value(QStringLiteral("host")).toString().trimmed();
    const int port = obj.value(QStringLiteral("port")).toInt(22);
    p.port = (port > 0 && port <= 65535) ? static_cast<quint16>(port) : 22;
    p.user = obj.value(QStringLiteral("user")).toString();
    p.auth = authFromKey(obj.value(QStringLiteral("auth")).toString());
    p.identityFile = obj.value(QStringLiteral("identityFile")).toString();
    p.passwordSaved = obj.value(QStringLiteral("passwordSaved")).toBool(false);
    p.remoteCommand = obj.value(QStringLiteral("remoteCommand")).toString();
    p.startupCommand = obj.value(QStringLiteral("startupCommand")).toString();
    p.terminalType = obj.value(QStringLiteral("terminalType")).toString(p.terminalType);
    if (p.terminalType.trimmed().isEmpty()) {
        p.terminalType = QStringLiteral("xterm-256color");
    }
    p.keepAliveSeconds = std::clamp(obj.value(QStringLiteral("keepAliveSeconds")).toInt(p.keepAliveSeconds), 0, 3600);
    p.connectTimeoutSeconds =
        std::clamp(obj.value(QStringLiteral("connectTimeoutSeconds")).toInt(p.connectTimeoutSeconds), 1, 600);
    p.proxyJump = obj.value(QStringLiteral("proxyJump")).toString();
    const QJsonArray forwards = obj.value(QStringLiteral("localForwards")).toArray();
    for (const QJsonValue& v : forwards) {
        if (v.isObject()) {
            p.localForwards.append(SshLocalForward::fromJson(v.toObject()));
        }
    }
    p.compression = obj.value(QStringLiteral("compression")).toBool(false);
    p.knownHostsFile = obj.value(QStringLiteral("knownHostsFile")).toString();
    p.description = obj.value(QStringLiteral("description")).toString();
    const QString lastUsed = obj.value(QStringLiteral("lastUsed")).toString();
    if (!lastUsed.isEmpty()) {
        p.lastUsed = QDateTime::fromString(lastUsed, Qt::ISODateWithMs);
        if (!p.lastUsed.isValid()) {
            p.lastUsed = QDateTime::fromString(lastUsed, Qt::ISODate);
        }
    }
    return p;
}

QString SshProfile::displayTarget() const
{
    QString text = bracketHost(host);
    if (!user.isEmpty()) {
        text.prepend(user + QLatin1Char('@'));
    }
    if (port != 22) {
        text += QStringLiteral(":%1").arg(port);
    }
    return text;
}

QString SshProfile::displayName() const
{
    const QString trimmed = name.trimmed();
    return trimmed.isEmpty() ? displayTarget() : trimmed;
}

bool SshProfile::isValid() const
{
    return !host.trimmed().isEmpty() && port != 0;
}

bool SshProfile::parseTarget(const QString& text, SshProfile& out)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        return false;
    }

    if (t.startsWith(QLatin1String("ssh://"), Qt::CaseInsensitive)) {
        const QUrl url(t, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() || url.scheme().compare(QLatin1String("ssh"), Qt::CaseInsensitive) != 0) {
            return false;
        }
        const int port = url.port(22);
        if (port <= 0 || port > 65535) {
            return false;
        }
        out.host = url.host();
        out.port = static_cast<quint16>(port);
        out.user = url.userName();
        return true;
    }

    // [user@]host[:port]  |  [user@][v6]:port  |  [user@]v6-literal (no port)
    static const QRegularExpression re(
        QStringLiteral("^(?:([^@\\s/]+)@)?(?:\\[([0-9A-Fa-f:.%]+)\\]|([^\\s:@\\[\\]/]+))(?::([0-9]{1,5}))?$"));
    const QRegularExpressionMatch m = re.match(t);
    QString user;
    QString host;
    quint16 port = 22;
    if (m.hasMatch()) {
        user = m.captured(1);
        host = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
        if (!m.captured(4).isEmpty()) {
            const int p = m.captured(4).toInt();
            if (p <= 0 || p > 65535) {
                return false;
            }
            port = static_cast<quint16>(p);
        }
    } else {
        // Bare IPv6 literal such as "::1" or "root@fe80::1%eth0" (port cannot be given without brackets).
        QString rest = t;
        const int at = rest.indexOf(QLatin1Char('@'));
        if (at >= 0) {
            user = rest.left(at);
            rest = rest.mid(at + 1);
        }
        static const QRegularExpression v6(QStringLiteral("^[0-9A-Fa-f:.%]+$"));
        if (user.contains(QLatin1Char(' ')) || rest.count(QLatin1Char(':')) < 2 || !v6.match(rest).hasMatch()) {
            return false;
        }
        host = rest;
    }
    if (host.isEmpty()) {
        return false;
    }
    out.host = host;
    out.port = port;
    out.user = user;
    return true;
}

QString SshProfile::authText(Auth auth)
{
    // SshProfile is a plain struct (no Q_OBJECT, no tr()): translate with an explicit context
    // so lupdate files the labels under "SshProfile" without guessing.
    switch (auth) {
    case Auth::Auto:
        return QCoreApplication::translate("SshProfile", "Automatic");
    case Auth::PublicKey:
        return QCoreApplication::translate("SshProfile", "Public key");
    case Auth::Password:
        return QCoreApplication::translate("SshProfile", "Password");
    case Auth::KeyboardInteractive:
        return QCoreApplication::translate("SshProfile", "Keyboard-interactive");
    case Auth::Agent:
        return QCoreApplication::translate("SshProfile", "SSH agent");
    }
    return QString();
}

QString SshProfile::authKey(Auth auth)
{
    switch (auth) {
    case Auth::Auto:
        return QStringLiteral("auto");
    case Auth::PublicKey:
        return QStringLiteral("publickey");
    case Auth::Password:
        return QStringLiteral("password");
    case Auth::KeyboardInteractive:
        return QStringLiteral("keyboard-interactive");
    case Auth::Agent:
        return QStringLiteral("agent");
    }
    return QStringLiteral("auto");
}

SshProfile::Auth SshProfile::authFromKey(const QString& key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("publickey")) {
        return Auth::PublicKey;
    }
    if (k == QLatin1String("password")) {
        return Auth::Password;
    }
    if (k == QLatin1String("keyboard-interactive")) {
        return Auth::KeyboardInteractive;
    }
    if (k == QLatin1String("agent")) {
        return Auth::Agent;
    }
    return Auth::Auto;
}

bool SshProfile::operator==(const SshProfile& other) const
{
    // lastUsed is bookkeeping, not part of the profile's identity/content.
    return id == other.id && name == other.name && host == other.host && port == other.port && user == other.user
        && auth == other.auth && identityFile == other.identityFile && passwordSaved == other.passwordSaved
        && remoteCommand == other.remoteCommand && startupCommand == other.startupCommand
        && terminalType == other.terminalType && keepAliveSeconds == other.keepAliveSeconds
        && connectTimeoutSeconds == other.connectTimeoutSeconds && proxyJump == other.proxyJump
        && localForwards == other.localForwards && compression == other.compression
        && knownHostsFile == other.knownHostsFile && description == other.description;
}

// ---------------------------------------------------------------------------
// SshProfileStore
// ---------------------------------------------------------------------------

SshProfileStore::SshProfileStore(QObject* parent)
    : QObject(parent)
{
}

QString SshProfileStore::defaultFilePath()
{
    return AppSettings::dataDirectory() + QStringLiteral("/ssh_profiles.json");
}

bool SshProfileStore::load(const QString& path)
{
    const QString file = path.isEmpty() ? defaultFilePath() : path;
    QList<SshProfile> profiles;
    QStringList recent;

    QFile f(file);
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) {
            qCWarning(lcApp) << "cannot read SSH profiles:" << file << f.errorString();
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            qCWarning(lcApp) << "SSH profiles file is not valid JSON:" << file << parseError.errorString();
            return false;
        }
        const QJsonObject root = doc.object();
        const QJsonArray array = root.value(QStringLiteral("profiles")).toArray();
        for (const QJsonValue& v : array) {
            if (!v.isObject()) {
                continue;
            }
            SshProfile p = SshProfile::fromJson(v.toObject());
            if (p.id.isEmpty()) {
                p.id = newProfileId();
            }
            profiles.append(p);
        }
        const QJsonArray recentArray = root.value(QStringLiteral("recentTargets")).toArray();
        for (const QJsonValue& v : recentArray) {
            const QString target = v.toString().trimmed();
            if (!target.isEmpty() && !recent.contains(target)) {
                recent.append(target);
            }
        }
        while (recent.size() > kMaxRecentTargets) {
            recent.removeLast();
        }
    }

    std::sort(profiles.begin(), profiles.end(), lessByName);
    const bool different = (profiles != m_profiles) || (recent != m_recent);
    m_profiles = profiles;
    m_recent = recent;
    if (different) {
        emit changed();
    }
    return true;
}

bool SshProfileStore::save(const QString& path) const
{
    const QString file = path.isEmpty() ? defaultFilePath() : path;
    const QFileInfo info(file);
    if (!QDir().mkpath(info.absolutePath())) {
        qCWarning(lcApp) << "cannot create directory for SSH profiles:" << info.absolutePath();
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kStoreVersion);
    QJsonArray array;
    for (const SshProfile& p : m_profiles) {
        array.append(p.toJson());
    }
    root.insert(QStringLiteral("profiles"), array);
    root.insert(QStringLiteral("recentTargets"), QJsonArray::fromStringList(m_recent));

    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcApp) << "cannot write SSH profiles:" << file << out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        qCWarning(lcApp) << "cannot commit SSH profiles:" << file << out.errorString();
        return false;
    }
    return true;
}

QList<SshProfile> SshProfileStore::profiles() const
{
    return m_profiles;
}

void SshProfileStore::setProfiles(const QList<SshProfile>& profiles)
{
    QList<SshProfile> normalized = profiles;
    for (SshProfile& p : normalized) {
        if (p.id.isEmpty()) {
            p.id = newProfileId();
        }
    }
    std::sort(normalized.begin(), normalized.end(), lessByName);
    if (normalized == m_profiles) {
        return;
    }
    m_profiles = normalized;
    emit changed();
}

std::optional<SshProfile> SshProfileStore::profile(const QString& id) const
{
    if (id.isEmpty()) {
        return std::nullopt;
    }
    for (const SshProfile& p : m_profiles) {
        if (p.id == id) {
            return p;
        }
    }
    return std::nullopt;
}

std::optional<SshProfile> SshProfileStore::profileByName(const QString& name) const
{
    for (const SshProfile& p : m_profiles) {
        if (QString::compare(p.displayName(), name, Qt::CaseInsensitive) == 0) {
            return p;
        }
    }
    return std::nullopt;
}

SshProfile SshProfileStore::upsert(SshProfile profile)
{
    if (profile.id.isEmpty()) {
        profile.id = newProfileId();
    }
    bool replaced = false;
    for (SshProfile& p : m_profiles) {
        if (p.id == profile.id) {
            p = profile;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_profiles.append(profile);
    }
    std::sort(m_profiles.begin(), m_profiles.end(), lessByName);
    emit changed();
    return profile;
}

bool SshProfileStore::remove(const QString& id)
{
    const auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                                 [&id](const SshProfile& p) { return p.id == id; });
    if (it == m_profiles.end()) {
        return false;
    }
    m_profiles.erase(it);
    emit changed();
    return true;
}

void SshProfileStore::touch(const QString& id)
{
    for (SshProfile& p : m_profiles) {
        if (p.id == id) {
            p.lastUsed = QDateTime::currentDateTime();
            emit changed();
            return;
        }
    }
}

QStringList SshProfileStore::recentTargets() const
{
    return m_recent;
}

void SshProfileStore::addRecentTarget(const QString& target)
{
    const QString t = target.trimmed();
    if (t.isEmpty()) {
        return;
    }
    m_recent.removeAll(t);
    m_recent.prepend(t);
    while (m_recent.size() > kMaxRecentTargets) {
        m_recent.removeLast();
    }
    emit changed();
}

bool SshProfileStore::importFromFile(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Cannot open %1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        }
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = tr("%1 is not valid JSON: %2").arg(QDir::toNativeSeparators(path), parseError.errorString());
        }
        return false;
    }
    QJsonArray array;
    if (doc.isArray()) {
        array = doc.array();
    } else if (doc.isObject()) {
        array = doc.object().value(QStringLiteral("profiles")).toArray();
    }
    if (array.isEmpty()) {
        if (error) {
            *error = tr("%1 contains no SSH profiles").arg(QDir::toNativeSeparators(path));
        }
        return false;
    }

    int imported = 0;
    for (const QJsonValue& v : array) {
        if (!v.isObject()) {
            continue;
        }
        SshProfile p = SshProfile::fromJson(v.toObject());
        if (!p.isValid()) {
            continue;
        }
        if (p.id.isEmpty()) {
            p.id = newProfileId();
        }
        p.passwordSaved = false;   // secrets are never part of an export
        bool replaced = false;
        for (SshProfile& existing : m_profiles) {
            if (existing.id == p.id) {
                existing = p;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            m_profiles.append(p);
        }
        ++imported;
    }
    if (imported == 0) {
        if (error) {
            *error = tr("%1 contains no valid SSH profiles").arg(QDir::toNativeSeparators(path));
        }
        return false;
    }
    std::sort(m_profiles.begin(), m_profiles.end(), lessByName);
    emit changed();
    qCInfo(lcApp) << "imported" << imported << "SSH profiles from" << path;
    return true;
}

bool SshProfileStore::exportToFile(const QString& path, QString* error) const
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), kStoreVersion);
    QJsonArray array;
    for (SshProfile p : m_profiles) {
        p.passwordSaved = false;
        array.append(p.toJson());
    }
    root.insert(QStringLiteral("profiles"), array);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(path), out.errorString());
        }
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (error) {
            *error = tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(path), out.errorString());
        }
        return false;
    }
    return true;
}
