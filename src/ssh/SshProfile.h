#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>
#include <QDateTime>
#include <optional>

/**
 * One local port forward (ssh -L [bind:]localPort:remoteHost:remotePort).
 */
struct SshLocalForward
{
    quint16 localPort = 0;
    QString remoteHost = QStringLiteral("127.0.0.1");
    quint16 remotePort = 0;
    QString bindAddress = QStringLiteral("127.0.0.1");   ///< "0.0.0.0" to expose on the LAN

    QJsonObject toJson() const;
    static SshLocalForward fromJson(const QJsonObject& obj);
    QString displayText() const;                        ///< "8080 -> 127.0.0.1:80"
    bool operator==(const SshLocalForward& other) const;
    bool operator!=(const SshLocalForward& other) const { return !(*this == other); }
};

/**
 * A saved SSH target. Everything needed to open a shell without asking questions, except
 * secrets, which live in SecretStore under the profile id (see passwordSaved).
 */
struct SshProfile
{
    enum class Auth {
        Auto,                ///< agent, then default identity files, then password / keyboard-interactive prompts
        PublicKey,           ///< identityFile (passphrase prompted or from SecretStore)
        Password,            ///< password prompted or from SecretStore
        KeyboardInteractive, ///< server-driven prompts (PAM / OTP)
        Agent                ///< only ssh-agent / Pageant
    };

    QString id;                   ///< UUID without braces; empty for an ad-hoc target typed in the bar
    QString name;                 ///< display name; empty -> displayTarget()
    QString host;
    quint16 port = 22;
    QString user;                 ///< empty -> the local user name
    Auth auth = Auth::Auto;
    QString identityFile;         ///< private key path (PublicKey) or optional override for Auto
    bool passwordSaved = false;   ///< SecretStore holds the password / key passphrase for this id
    QString remoteCommand;        ///< empty -> interactive shell; otherwise `exec` with a PTY
    QString startupCommand;       ///< typed into the shell after login (e.g. "cd /oem && ls")
    QString terminalType = QStringLiteral("xterm-256color");
    int keepAliveSeconds = 30;    ///< 0 = off
    int connectTimeoutSeconds = 15;
    QString proxyJump;            ///< "[user@]host[:port]" to hop through (empty = direct)
    QList<SshLocalForward> localForwards;
    bool compression = false;
    QString knownHostsFile;       ///< empty -> SshConnection::defaultKnownHostsFile()
    QString description;
    QDateTime lastUsed;

    QJsonObject toJson() const;                         ///< never contains secrets
    static SshProfile fromJson(const QJsonObject& obj); ///< missing fields -> defaults above

    QString displayTarget() const;                      ///< "user@host:port" ("host" when user empty, port omitted when 22)
    QString displayName() const;                        ///< name or displayTarget()
    bool isValid() const;                               ///< host not empty, port != 0
    /// Parse "user@host:port", "ssh://user@host:port", "host:port", "[::1]:2222", "host" into
    /// host/port/user of `out` (other fields untouched). Returns false when the text is not a target.
    static bool parseTarget(const QString& text, SshProfile& out);
    static QString authText(Auth auth);                 ///< translated label
    static QString authKey(Auth auth);                  ///< "auto" | "publickey" | "password" | "keyboard-interactive" | "agent"
    static Auth authFromKey(const QString& key);

    bool operator==(const SshProfile& other) const;
    bool operator!=(const SshProfile& other) const { return !(*this == other); }
};

/**
 * Persists the profiles as JSON at AppSettings::dataDirectory()/ssh_profiles.json:
 *   { "version": 1, "profiles": [ {...}, ... ], "recentTargets": ["root@10.0.0.24", ...] }
 * recentTargets keeps the last 10 ad-hoc targets typed into the connection bar (most recent first).
 */
class SshProfileStore : public QObject
{
    Q_OBJECT
public:
    explicit SshProfileStore(QObject* parent = nullptr);

    static QString defaultFilePath();

    bool load(const QString& path = QString());          ///< missing file -> empty list, returns true
    bool save(const QString& path = QString()) const;

    QList<SshProfile> profiles() const;                  ///< sorted by name (case-insensitive)
    void setProfiles(const QList<SshProfile>& profiles); ///< emits changed() if different
    std::optional<SshProfile> profile(const QString& id) const;
    std::optional<SshProfile> profileByName(const QString& name) const;
    /// Insert or replace by id; an empty id gets a new UUID. Returns the stored profile.
    SshProfile upsert(SshProfile profile);
    bool remove(const QString& id);
    void touch(const QString& id);                       ///< lastUsed = now

    QStringList recentTargets() const;
    void addRecentTarget(const QString& target);         ///< de-duplicated, capped at 10

    bool importFromFile(const QString& path, QString* error = nullptr);   ///< merges by id
    bool exportToFile(const QString& path, QString* error = nullptr) const;

signals:
    void changed();

private:
    QList<SshProfile> m_profiles;
    QStringList m_recent;
};
