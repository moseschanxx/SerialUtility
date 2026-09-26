#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QStandardPaths>

#include "app/AppSettings.h"
#include "ssh/SshProfile.h"

/// SshLocalForward / SshProfile value semantics, target parsing and the SshProfileStore JSON
/// persistence, all inside a temporary directory (the real data directory is never touched).
class Tst_sshprofile : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    // ---- SshLocalForward / SshProfile ------------------------------------------------
    void forwardJsonRoundTrip();
    void forwardFromJsonDefaultsAndClamps();
    void forwardDisplayText();
    void profileJsonRoundTrip();
    void profileFromJsonDefaults();
    void profileFromJsonClamps();
    void displayTargetAndName();
    void parseTarget_data();
    void parseTarget();
    void parseTargetLeavesOutputUntouchedOnFailure();
    void authKeyRoundTrip();
    void equalityIgnoresLastUsed();
    void isValid();

    // ---- SshProfileStore ----------------------------------------------------------------
    void storeLoadMissingFile();
    void storeUpsertAssignsIdsAndSorts();
    void storeLookup();
    void storeRemove();
    void storeTouch();
    void storeRecentTargets();
    void storeSaveLoadRoundTrip();
    void storeChangedEmissions();
    void storeImportObjectForm();
    void storeImportBareArray();
    void storeImportMergesById();
    void storeImportErrors();
    void storeExportExcludesRecentAndSecrets();
    void storeCorruptJson();
    void storeDefaultFilePath();

private:
    static SshProfile sampleProfile(const QString& name);
    static SshLocalForward sampleForward(quint16 local, quint16 remote);
    QString writeFile(const QString& name, const QByteArray& content);
    QTemporaryDir m_dir;
};

void Tst_sshprofile::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-sshprofile"));
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.filePath(QStringLiteral("settings")));
}

SshLocalForward Tst_sshprofile::sampleForward(quint16 local, quint16 remote)
{
    SshLocalForward fwd;
    fwd.localPort = local;
    fwd.remoteHost = QStringLiteral("10.0.0.5");
    fwd.remotePort = remote;
    fwd.bindAddress = QStringLiteral("0.0.0.0");
    return fwd;
}

SshProfile Tst_sshprofile::sampleProfile(const QString& name)
{
    SshProfile p;
    p.id = QStringLiteral("id-") + name.toLower();
    p.name = name;
    p.host = QStringLiteral("192.168.100.2");
    p.port = 2222;
    p.user = QStringLiteral("root");
    p.auth = SshProfile::Auth::PublicKey;
    p.identityFile = QStringLiteral("~/.ssh/id_ed25519");
    p.passwordSaved = true;
    p.remoteCommand = QStringLiteral("tail -f /var/log/messages");
    p.startupCommand = QStringLiteral("cd /oem && ls");
    p.terminalType = QStringLiteral("xterm");
    p.keepAliveSeconds = 45;
    p.connectTimeoutSeconds = 7;
    p.proxyJump = QStringLiteral("jump@bastion:2200");
    p.localForwards = {sampleForward(8080, 80), sampleForward(5900, 5901)};
    p.compression = true;
    p.knownHostsFile = QStringLiteral("D:/tmp/known_hosts");
    p.description = QStringLiteral("Luckfox Pico Ultra over the direct cable");
    p.lastUsed = QDateTime(QDate(2026, 9, 21), QTime(13, 45, 30, 250), QTimeZone::UTC);
    return p;
}

QString Tst_sshprofile::writeFile(const QString& name, const QByteArray& content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(content);
    return path;
}

// ---------------------------------------------------------------------------------------
// SshLocalForward / SshProfile
// ---------------------------------------------------------------------------------------

void Tst_sshprofile::forwardJsonRoundTrip()
{
    const SshLocalForward fwd = sampleForward(8080, 80);
    const QJsonObject json = fwd.toJson();
    QCOMPARE(json.value(QStringLiteral("localPort")).toInt(), 8080);
    QCOMPARE(json.value(QStringLiteral("remoteHost")).toString(), QStringLiteral("10.0.0.5"));
    QCOMPARE(json.value(QStringLiteral("remotePort")).toInt(), 80);
    QCOMPARE(json.value(QStringLiteral("bindAddress")).toString(), QStringLiteral("0.0.0.0"));
    const SshLocalForward back = SshLocalForward::fromJson(json);
    QVERIFY(back == fwd);
    QVERIFY(!(back != fwd));
}

void Tst_sshprofile::forwardFromJsonDefaultsAndClamps()
{
    const SshLocalForward empty = SshLocalForward::fromJson(QJsonObject());
    QCOMPARE(empty.localPort, quint16(0));
    QCOMPARE(empty.remotePort, quint16(0));
    QCOMPARE(empty.remoteHost, QStringLiteral("127.0.0.1"));
    QCOMPARE(empty.bindAddress, QStringLiteral("127.0.0.1"));

    QJsonObject bad;
    bad.insert(QStringLiteral("localPort"), 70000);
    bad.insert(QStringLiteral("remotePort"), -5);
    bad.insert(QStringLiteral("bindAddress"), QStringLiteral("   "));
    bad.insert(QStringLiteral("remoteHost"), QStringLiteral("board"));
    const SshLocalForward clamped = SshLocalForward::fromJson(bad);
    QCOMPARE(clamped.localPort, quint16(0));
    QCOMPARE(clamped.remotePort, quint16(0));
    QCOMPARE(clamped.bindAddress, QStringLiteral("127.0.0.1"));
    QCOMPARE(clamped.remoteHost, QStringLiteral("board"));
}

void Tst_sshprofile::forwardDisplayText()
{
    SshLocalForward fwd;
    fwd.localPort = 8080;
    fwd.remoteHost = QStringLiteral("127.0.0.1");
    fwd.remotePort = 80;
    QCOMPARE(fwd.displayText(), QStringLiteral("8080 -> 127.0.0.1:80"));
    fwd.bindAddress = QStringLiteral("0.0.0.0");
    QCOMPARE(fwd.displayText(), QStringLiteral("0.0.0.0:8080 -> 127.0.0.1:80"));
    fwd.bindAddress = QStringLiteral("localhost");
    fwd.remoteHost = QStringLiteral("::1");
    QCOMPARE(fwd.displayText(), QStringLiteral("8080 -> [::1]:80"));
}

void Tst_sshprofile::profileJsonRoundTrip()
{
    const SshProfile p = sampleProfile(QStringLiteral("Luckfox"));
    const QJsonObject json = p.toJson();
    QCOMPARE(json.value(QStringLiteral("auth")).toString(), QStringLiteral("publickey"));
    QCOMPARE(json.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(json.value(QStringLiteral("localForwards")).toArray().size(), 2);
    QVERIFY(json.contains(QStringLiteral("lastUsed")));
    QVERIFY(!json.contains(QStringLiteral("password")));

    const SshProfile back = SshProfile::fromJson(json);
    QVERIFY(back == p);
    QCOMPARE(back.id, p.id);
    QCOMPARE(back.name, p.name);
    QCOMPARE(back.host, p.host);
    QCOMPARE(back.port, p.port);
    QCOMPARE(back.user, p.user);
    QCOMPARE(back.auth, p.auth);
    QCOMPARE(back.identityFile, p.identityFile);
    QCOMPARE(back.passwordSaved, p.passwordSaved);
    QCOMPARE(back.remoteCommand, p.remoteCommand);
    QCOMPARE(back.startupCommand, p.startupCommand);
    QCOMPARE(back.terminalType, p.terminalType);
    QCOMPARE(back.keepAliveSeconds, p.keepAliveSeconds);
    QCOMPARE(back.connectTimeoutSeconds, p.connectTimeoutSeconds);
    QCOMPARE(back.proxyJump, p.proxyJump);
    QCOMPARE(back.localForwards, p.localForwards);
    QCOMPARE(back.compression, p.compression);
    QCOMPARE(back.knownHostsFile, p.knownHostsFile);
    QCOMPARE(back.description, p.description);
    QVERIFY(back.lastUsed.isValid());
    QCOMPARE(back.lastUsed.toMSecsSinceEpoch(), p.lastUsed.toMSecsSinceEpoch());

    // A profile without lastUsed writes no key and reads back invalid.
    SshProfile fresh = p;
    fresh.lastUsed = QDateTime();
    QVERIFY(!fresh.toJson().contains(QStringLiteral("lastUsed")));
    QVERIFY(!SshProfile::fromJson(fresh.toJson()).lastUsed.isValid());
}

void Tst_sshprofile::profileFromJsonDefaults()
{
    const SshProfile p = SshProfile::fromJson(QJsonObject());
    const SshProfile defaults;
    QVERIFY(p.id.isEmpty());
    QVERIFY(p.name.isEmpty());
    QVERIFY(p.host.isEmpty());
    QCOMPARE(p.port, quint16(22));
    QVERIFY(p.user.isEmpty());
    QCOMPARE(p.auth, SshProfile::Auth::Auto);
    QVERIFY(p.identityFile.isEmpty());
    QCOMPARE(p.passwordSaved, false);
    QCOMPARE(p.terminalType, QStringLiteral("xterm-256color"));
    QCOMPARE(p.keepAliveSeconds, defaults.keepAliveSeconds);
    QCOMPARE(p.connectTimeoutSeconds, defaults.connectTimeoutSeconds);
    QVERIFY(p.proxyJump.isEmpty());
    QVERIFY(p.localForwards.isEmpty());
    QCOMPARE(p.compression, false);
    QVERIFY(p.knownHostsFile.isEmpty());
    QVERIFY(!p.lastUsed.isValid());
    QVERIFY(p == defaults);
}

void Tst_sshprofile::profileFromJsonClamps()
{
    QJsonObject json;
    json.insert(QStringLiteral("host"), QStringLiteral("  board.local  "));
    json.insert(QStringLiteral("port"), 0);
    json.insert(QStringLiteral("auth"), QStringLiteral("bogus"));
    json.insert(QStringLiteral("terminalType"), QStringLiteral("   "));
    json.insert(QStringLiteral("keepAliveSeconds"), -5);
    json.insert(QStringLiteral("connectTimeoutSeconds"), 100000);
    json.insert(QStringLiteral("lastUsed"), QStringLiteral("not a date"));
    QJsonArray forwards;
    forwards.append(QStringLiteral("not an object"));
    forwards.append(sampleForward(1, 2).toJson());
    json.insert(QStringLiteral("localForwards"), forwards);

    SshProfile p = SshProfile::fromJson(json);
    QCOMPARE(p.host, QStringLiteral("board.local"));
    QCOMPARE(p.port, quint16(22));
    QCOMPARE(p.auth, SshProfile::Auth::Auto);
    QCOMPARE(p.terminalType, QStringLiteral("xterm-256color"));
    QCOMPARE(p.keepAliveSeconds, 0);
    QCOMPARE(p.connectTimeoutSeconds, 600);
    QVERIFY(!p.lastUsed.isValid());
    QCOMPARE(p.localForwards.size(), 1);
    QCOMPARE(p.localForwards.first().localPort, quint16(1));

    json.insert(QStringLiteral("port"), 70000);
    json.insert(QStringLiteral("keepAliveSeconds"), 100000);
    json.insert(QStringLiteral("connectTimeoutSeconds"), 0);
    json.insert(QStringLiteral("port"), QStringLiteral("22"));   // wrong type -> default
    p = SshProfile::fromJson(json);
    QCOMPARE(p.port, quint16(22));
    QCOMPARE(p.keepAliveSeconds, 3600);
    QCOMPARE(p.connectTimeoutSeconds, 1);

    json.insert(QStringLiteral("port"), 65536);
    QCOMPARE(SshProfile::fromJson(json).port, quint16(22));
    json.insert(QStringLiteral("port"), 65535);
    QCOMPARE(SshProfile::fromJson(json).port, quint16(65535));
}

void Tst_sshprofile::displayTargetAndName()
{
    SshProfile p;
    p.host = QStringLiteral("10.0.0.24");
    QCOMPARE(p.displayTarget(), QStringLiteral("10.0.0.24"));
    QCOMPARE(p.displayName(), QStringLiteral("10.0.0.24"));

    p.user = QStringLiteral("root");
    QCOMPARE(p.displayTarget(), QStringLiteral("root@10.0.0.24"));
    p.port = 2222;
    QCOMPARE(p.displayTarget(), QStringLiteral("root@10.0.0.24:2222"));
    p.user.clear();
    QCOMPARE(p.displayTarget(), QStringLiteral("10.0.0.24:2222"));

    p.host = QStringLiteral("::1");
    p.port = 22;
    QCOMPARE(p.displayTarget(), QStringLiteral("[::1]"));
    p.user = QStringLiteral("root");
    QCOMPARE(p.displayTarget(), QStringLiteral("root@[::1]"));
    p.host = QStringLiteral("fe80::1");
    p.port = 2222;
    QCOMPARE(p.displayTarget(), QStringLiteral("root@[fe80::1]:2222"));

    p.name = QStringLiteral("  Luckfox  ");
    QCOMPARE(p.displayName(), QStringLiteral("Luckfox"));
    QCOMPARE(p.displayTarget(), QStringLiteral("root@[fe80::1]:2222"));
    p.name = QStringLiteral("   ");
    QCOMPARE(p.displayName(), p.displayTarget());
}

void Tst_sshprofile::parseTarget_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<QString>("user");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");

    QTest::newRow("host") << "host" << true << "" << "host" << 22;
    QTest::newRow("user@host") << "user@host" << true << "user" << "host" << 22;
    QTest::newRow("user@host:2222") << "user@host:2222" << true << "user" << "host" << 2222;
    QTest::newRow("ssh://user@host:2222") << "ssh://user@host:2222" << true << "user" << "host" << 2222;
    QTest::newRow("ssh://host") << "ssh://host" << true << "" << "host" << 22;
    QTest::newRow("ssh://[::1]:2222") << "ssh://[::1]:2222" << true << "" << "::1" << 2222;
    QTest::newRow("[::1]:2222") << "[::1]:2222" << true << "" << "::1" << 2222;
    QTest::newRow("root@[fe80::1]:22") << "root@[fe80::1]:22" << true << "root" << "fe80::1" << 22;
    QTest::newRow("::1") << "::1" << true << "" << "::1" << 22;
    QTest::newRow("user@::1") << "user@::1" << true << "user" << "::1" << 22;
    QTest::newRow("padded") << "  root@10.0.0.24:22  " << true << "root" << "10.0.0.24" << 22;
    QTest::newRow("ipv4:port") << "10.0.0.24:2222" << true << "" << "10.0.0.24" << 2222;

    QTest::newRow("empty") << "" << false << "" << "" << 0;
    QTest::newRow("user@") << "user@" << false << "" << "" << 0;
    QTest::newRow(":22") << ":22" << false << "" << "" << 0;
    QTest::newRow("host:0") << "host:0" << false << "" << "" << 0;
    QTest::newRow("host:70000") << "host:70000" << false << "" << "" << 0;
    QTest::newRow("a b") << "a b" << false << "" << "" << 0;
    QTest::newRow("ssh://") << "ssh://" << false << "" << "" << 0;
}

void Tst_sshprofile::parseTarget()
{
    QFETCH(QString, text);
    QFETCH(bool, ok);
    QFETCH(QString, user);
    QFETCH(QString, host);
    QFETCH(int, port);

    SshProfile out;
    QCOMPARE(SshProfile::parseTarget(text, out), ok);
    if (ok) {
        QCOMPARE(out.user, user);
        QCOMPARE(out.host, host);
        QCOMPARE(int(out.port), port);
        QVERIFY(out.isValid());
    }
}

void Tst_sshprofile::parseTargetLeavesOutputUntouchedOnFailure()
{
    SshProfile out;
    out.name = QStringLiteral("keep");
    out.host = QStringLiteral("keep.host");
    out.port = 1234;
    out.user = QStringLiteral("keeper");
    out.auth = SshProfile::Auth::Password;
    QVERIFY(!SshProfile::parseTarget(QStringLiteral("host:0"), out));
    QCOMPARE(out.host, QStringLiteral("keep.host"));
    QCOMPARE(out.port, quint16(1234));
    QCOMPARE(out.user, QStringLiteral("keeper"));

    // Success only touches host / port / user.
    QVERIFY(SshProfile::parseTarget(QStringLiteral("root@board"), out));
    QCOMPARE(out.name, QStringLiteral("keep"));
    QCOMPARE(out.auth, SshProfile::Auth::Password);
    QCOMPARE(out.host, QStringLiteral("board"));
    QCOMPARE(out.port, quint16(22));
    QCOMPARE(out.user, QStringLiteral("root"));
}

void Tst_sshprofile::authKeyRoundTrip()
{
    const QList<SshProfile::Auth> all = {SshProfile::Auth::Auto, SshProfile::Auth::PublicKey, SshProfile::Auth::Password,
                                         SshProfile::Auth::KeyboardInteractive, SshProfile::Auth::Agent};
    QStringList keys;
    for (SshProfile::Auth auth : all) {
        const QString key = SshProfile::authKey(auth);
        QVERIFY(!key.isEmpty());
        QVERIFY(!keys.contains(key));
        keys.append(key);
        QCOMPARE(SshProfile::authFromKey(key), auth);
        QCOMPARE(SshProfile::authFromKey(key.toUpper() + QStringLiteral("  ")), auth);
        QVERIFY(!SshProfile::authText(auth).isEmpty());
    }
    QCOMPARE(SshProfile::authKey(SshProfile::Auth::KeyboardInteractive), QStringLiteral("keyboard-interactive"));
    QCOMPARE(SshProfile::authFromKey(QStringLiteral("unknown")), SshProfile::Auth::Auto);
    QCOMPARE(SshProfile::authFromKey(QString()), SshProfile::Auth::Auto);
}

void Tst_sshprofile::equalityIgnoresLastUsed()
{
    const SshProfile a = sampleProfile(QStringLiteral("A"));
    SshProfile b = a;
    QVERIFY(a == b);
    b.lastUsed = QDateTime::currentDateTime().addDays(3);
    QVERIFY(a == b);
    b.lastUsed = QDateTime();
    QVERIFY(a == b);

    b.description += QStringLiteral("!");
    QVERIFY(a != b);
    b = a;
    b.localForwards.removeLast();
    QVERIFY(a != b);
    b = a;
    b.passwordSaved = false;
    QVERIFY(a != b);
    b = a;
    b.port = 22;
    QVERIFY(a != b);
}

void Tst_sshprofile::isValid()
{
    SshProfile p;
    QVERIFY(!p.isValid());
    p.host = QStringLiteral("   ");
    QVERIFY(!p.isValid());
    p.host = QStringLiteral("board");
    QVERIFY(p.isValid());
    p.port = 0;
    QVERIFY(!p.isValid());
}

// ---------------------------------------------------------------------------------------
// SshProfileStore
// ---------------------------------------------------------------------------------------

void Tst_sshprofile::storeLoadMissingFile()
{
    SshProfileStore store;
    QSignalSpy changed(&store, &SshProfileStore::changed);
    QVERIFY(store.load(m_dir.filePath(QStringLiteral("does-not-exist.json"))));
    QVERIFY(store.profiles().isEmpty());
    QVERIFY(store.recentTargets().isEmpty());
    QCOMPARE(changed.count(), 0);
}

void Tst_sshprofile::storeUpsertAssignsIdsAndSorts()
{
    SshProfileStore store;
    SshProfile zeta = sampleProfile(QStringLiteral("zeta"));
    zeta.id.clear();
    SshProfile alpha = sampleProfile(QStringLiteral("Alpha"));
    alpha.id.clear();
    SshProfile beta = sampleProfile(QStringLiteral("beta"));
    beta.id.clear();
    SshProfile unnamed = sampleProfile(QString());
    unnamed.id.clear();
    unnamed.name.clear();               // sorts by displayTarget() "root@192.168.100.2:2222"

    const SshProfile storedZeta = store.upsert(zeta);
    const SshProfile storedAlpha = store.upsert(alpha);
    const SshProfile storedBeta = store.upsert(beta);
    const SshProfile storedUnnamed = store.upsert(unnamed);
    QVERIFY(!storedZeta.id.isEmpty());
    QVERIFY(!storedAlpha.id.isEmpty());
    QVERIFY(!storedBeta.id.isEmpty());
    QVERIFY(storedZeta.id != storedAlpha.id);
    QVERIFY(storedAlpha.id != storedBeta.id);

    QStringList names;
    for (const SshProfile& p : store.profiles()) {
        names.append(p.displayName());
    }
    QCOMPARE(names, QStringList({QStringLiteral("Alpha"), QStringLiteral("beta"), QStringLiteral("root@192.168.100.2:2222"),
                                 QStringLiteral("zeta")}));

    // Replace by id keeps the count and re-sorts.
    SshProfile renamed = storedZeta;
    renamed.name = QStringLiteral("aardvark");
    const SshProfile replaced = store.upsert(renamed);
    QCOMPARE(replaced.id, storedZeta.id);
    QCOMPARE(store.profiles().size(), 4);
    QCOMPARE(store.profiles().first().name, QStringLiteral("aardvark"));

    // setProfiles also assigns ids and sorts.
    SshProfileStore other;
    other.setProfiles({zeta, alpha});
    QCOMPARE(other.profiles().size(), 2);
    QCOMPARE(other.profiles().first().name, QStringLiteral("Alpha"));
    QVERIFY(!other.profiles().first().id.isEmpty());
}

void Tst_sshprofile::storeLookup()
{
    SshProfileStore store;
    const SshProfile a = store.upsert(sampleProfile(QStringLiteral("Luckfox")));
    const SshProfile b = store.upsert(sampleProfile(QStringLiteral("Lyra")));

    QVERIFY(store.profile(a.id).has_value());
    QCOMPARE(store.profile(a.id)->name, QStringLiteral("Luckfox"));
    QVERIFY(store.profile(b.id).has_value());
    QVERIFY(!store.profile(QStringLiteral("nope")).has_value());
    QVERIFY(!store.profile(QString()).has_value());

    QVERIFY(store.profileByName(QStringLiteral("lyra")).has_value());
    QCOMPARE(store.profileByName(QStringLiteral("LYRA"))->id, b.id);
    QVERIFY(!store.profileByName(QStringLiteral("Pico")).has_value());
}

void Tst_sshprofile::storeRemove()
{
    SshProfileStore store;
    const SshProfile a = store.upsert(sampleProfile(QStringLiteral("A")));
    const SshProfile b = store.upsert(sampleProfile(QStringLiteral("B")));
    QSignalSpy changed(&store, &SshProfileStore::changed);
    QVERIFY(store.remove(a.id));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.profiles().first().id, b.id);
    QVERIFY(!store.remove(a.id));
    QVERIFY(!store.remove(QString()));
    QCOMPARE(changed.count(), 1);
}

void Tst_sshprofile::storeTouch()
{
    SshProfileStore store;
    SshProfile p = sampleProfile(QStringLiteral("A"));
    p.lastUsed = QDateTime();
    const SshProfile stored = store.upsert(p);
    QVERIFY(!store.profile(stored.id)->lastUsed.isValid());
    QSignalSpy changed(&store, &SshProfileStore::changed);
    const QDateTime before = QDateTime::currentDateTime().addSecs(-1);
    store.touch(stored.id);
    QCOMPARE(changed.count(), 1);
    const QDateTime lastUsed = store.profile(stored.id)->lastUsed;
    QVERIFY(lastUsed.isValid());
    QVERIFY(lastUsed >= before);
    store.touch(QStringLiteral("unknown"));
    QCOMPARE(changed.count(), 1);
}

void Tst_sshprofile::storeRecentTargets()
{
    SshProfileStore store;
    for (int i = 1; i <= 12; ++i) {
        store.addRecentTarget(QStringLiteral("root@10.0.0.%1").arg(i));
    }
    QCOMPARE(store.recentTargets().size(), 10);
    QCOMPARE(store.recentTargets().first(), QStringLiteral("root@10.0.0.12"));
    QCOMPARE(store.recentTargets().last(), QStringLiteral("root@10.0.0.3"));

    store.addRecentTarget(QStringLiteral("  root@10.0.0.5  "));
    QCOMPARE(store.recentTargets().size(), 10);
    QCOMPARE(store.recentTargets().first(), QStringLiteral("root@10.0.0.5"));
    QCOMPARE(store.recentTargets().count(QStringLiteral("root@10.0.0.5")), 1);

    QSignalSpy changed(&store, &SshProfileStore::changed);
    store.addRecentTarget(QStringLiteral("   "));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(store.recentTargets().size(), 10);
}

void Tst_sshprofile::storeSaveLoadRoundTrip()
{
    const QString path = m_dir.filePath(QStringLiteral("sub/dir/ssh_profiles.json"));
    SshProfileStore a;
    a.upsert(sampleProfile(QStringLiteral("Luckfox")));
    SshProfile lyra = sampleProfile(QStringLiteral("Lyra"));
    lyra.auth = SshProfile::Auth::KeyboardInteractive;
    lyra.lastUsed = QDateTime();
    a.upsert(lyra);
    a.addRecentTarget(QStringLiteral("root@10.0.0.24"));
    a.addRecentTarget(QStringLiteral("pi@10.0.0.30:2222"));
    QVERIFY(a.save(path));
    QVERIFY(QFile::exists(path));

    SshProfileStore b;
    QSignalSpy changed(&b, &SshProfileStore::changed);
    QVERIFY(b.load(path));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(b.profiles(), a.profiles());
    QCOMPARE(b.recentTargets(), a.recentTargets());
    QCOMPARE(b.recentTargets().first(), QStringLiteral("pi@10.0.0.30:2222"));
    for (const SshProfile& p : a.profiles()) {
        const std::optional<SshProfile> loaded = b.profile(p.id);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->lastUsed.isValid(), p.lastUsed.isValid());
        if (p.lastUsed.isValid()) {
            QCOMPARE(loaded->lastUsed.toMSecsSinceEpoch(), p.lastUsed.toMSecsSinceEpoch());
        }
    }

    // Loading the same content again is not a change.
    QVERIFY(b.load(path));
    QCOMPARE(changed.count(), 1);

    // The file has the documented shape.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(root.value(QStringLiteral("profiles")).toArray().size(), 2);
    QCOMPARE(root.value(QStringLiteral("recentTargets")).toArray().size(), 2);
}

void Tst_sshprofile::storeChangedEmissions()
{
    SshProfileStore store;
    QSignalSpy changed(&store, &SshProfileStore::changed);
    const SshProfile a = store.upsert(sampleProfile(QStringLiteral("A")));
    QCOMPARE(changed.count(), 1);
    store.setProfiles(store.profiles());          // identical -> no emission
    QCOMPARE(changed.count(), 1);
    store.setProfiles({});
    QCOMPARE(changed.count(), 2);
    store.setProfiles({a});
    QCOMPARE(changed.count(), 3);
    store.addRecentTarget(QStringLiteral("x@y"));
    QCOMPARE(changed.count(), 4);
    store.touch(a.id);
    QCOMPARE(changed.count(), 5);
    QVERIFY(store.remove(a.id));
    QCOMPARE(changed.count(), 6);
    QVERIFY(store.load(m_dir.filePath(QStringLiteral("missing.json"))));   // empty -> recent list cleared
    QCOMPARE(changed.count(), 7);
    QVERIFY(store.load(m_dir.filePath(QStringLiteral("missing.json"))));   // still empty -> nothing
    QCOMPARE(changed.count(), 7);
}

void Tst_sshprofile::storeImportObjectForm()
{
    SshProfile p = sampleProfile(QStringLiteral("Imported"));
    p.passwordSaved = true;
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("profiles"), QJsonArray({p.toJson()}));
    root.insert(QStringLiteral("recentTargets"), QJsonArray({QStringLiteral("should@be.ignored")}));
    const QString path = writeFile(QStringLiteral("import-object.json"), QJsonDocument(root).toJson());

    SshProfileStore store;
    QSignalSpy changed(&store, &SshProfileStore::changed);
    QString error;
    QVERIFY2(store.importFromFile(path, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(changed.count(), 1);
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.profiles().first().id, p.id);
    QCOMPARE(store.profiles().first().name, QStringLiteral("Imported"));
    QCOMPARE(store.profiles().first().passwordSaved, false);
    QVERIFY(store.recentTargets().isEmpty());
}

void Tst_sshprofile::storeImportBareArray()
{
    SshProfile a = sampleProfile(QStringLiteral("Bare A"));
    a.id.clear();                          // gets a fresh id
    SshProfile invalid = sampleProfile(QStringLiteral("No host"));
    invalid.host.clear();                  // skipped
    const QJsonArray array({a.toJson(), invalid.toJson(), QStringLiteral("junk")});
    const QString path = writeFile(QStringLiteral("import-array.json"), QJsonDocument(array).toJson());

    SshProfileStore store;
    QVERIFY(store.importFromFile(path));
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.profiles().first().name, QStringLiteral("Bare A"));
    QVERIFY(!store.profiles().first().id.isEmpty());
}

void Tst_sshprofile::storeImportMergesById()
{
    SshProfileStore store;
    const SshProfile existing = store.upsert(sampleProfile(QStringLiteral("Existing")));
    const SshProfile other = store.upsert(sampleProfile(QStringLiteral("Other")));

    SshProfile updated = existing;
    updated.name = QStringLiteral("Existing (updated)");
    updated.port = 2022;
    SshProfile added = sampleProfile(QStringLiteral("Added"));
    added.id = QStringLiteral("added-id");
    const QString path = writeFile(QStringLiteral("import-merge.json"),
                                   QJsonDocument(QJsonArray({updated.toJson(), added.toJson()})).toJson());
    QVERIFY(store.importFromFile(path));
    QCOMPARE(store.profiles().size(), 3);
    QCOMPARE(store.profile(existing.id)->name, QStringLiteral("Existing (updated)"));
    QCOMPARE(store.profile(existing.id)->port, quint16(2022));
    QCOMPARE(store.profile(other.id)->name, QStringLiteral("Other"));
    QCOMPARE(store.profile(QStringLiteral("added-id"))->name, QStringLiteral("Added"));
    QCOMPARE(store.profiles().first().name, QStringLiteral("Added"));   // sorted after the merge
}

void Tst_sshprofile::storeImportErrors()
{
    SshProfileStore store;
    QString error;
    QVERIFY(!store.importFromFile(m_dir.filePath(QStringLiteral("missing-import.json")), &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!store.importFromFile(writeFile(QStringLiteral("import-bad.json"), "{ nope"), &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!store.importFromFile(writeFile(QStringLiteral("import-empty.json"), "{\"profiles\": []}"), &error));
    QVERIFY(!error.isEmpty());

    SshProfile invalid = sampleProfile(QStringLiteral("x"));
    invalid.host.clear();
    error.clear();
    QVERIFY(!store.importFromFile(writeFile(QStringLiteral("import-invalid.json"),
                                            QJsonDocument(QJsonArray({invalid.toJson()})).toJson()), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(store.profiles().isEmpty());
}

void Tst_sshprofile::storeExportExcludesRecentAndSecrets()
{
    SshProfileStore store;
    SshProfile p = sampleProfile(QStringLiteral("Secret"));
    p.passwordSaved = true;
    store.upsert(p);
    store.addRecentTarget(QStringLiteral("root@10.0.0.24"));
    const QString path = m_dir.filePath(QStringLiteral("export.json"));
    QString error;
    QVERIFY2(store.exportToFile(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(!root.contains(QStringLiteral("recentTargets")));
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    QCOMPARE(profiles.size(), 1);
    QCOMPARE(profiles.first().toObject().value(QStringLiteral("passwordSaved")).toBool(true), false);
    QCOMPARE(profiles.first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Secret"));
    // The store itself still remembers that a password is saved.
    QCOMPARE(store.profiles().first().passwordSaved, true);

    QVERIFY(!store.exportToFile(m_dir.filePath(QStringLiteral("no/such/dir/export.json")), &error));
    QVERIFY(!error.isEmpty());
}

void Tst_sshprofile::storeCorruptJson()
{
    SshProfileStore store;
    const SshProfile kept = store.upsert(sampleProfile(QStringLiteral("Kept")));
    store.addRecentTarget(QStringLiteral("kept@target"));
    QSignalSpy changed(&store, &SshProfileStore::changed);

    QVERIFY(!store.load(writeFile(QStringLiteral("corrupt.json"), "{ \"profiles\": [ oops")));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.profiles().first().id, kept.id);
    QCOMPARE(store.recentTargets(), QStringList{QStringLiteral("kept@target")});

    QVERIFY(!store.load(writeFile(QStringLiteral("array-root.json"), "[1, 2, 3]")));
    QCOMPARE(store.profiles().size(), 1);
}

void Tst_sshprofile::storeDefaultFilePath()
{
    const QString path = SshProfileStore::defaultFilePath();
    QVERIFY(path.startsWith(AppSettings::dataDirectory()));
    QVERIFY(path.endsWith(QStringLiteral("/ssh_profiles.json")));
    // Test mode keeps the data directory away from the real profile.
    QVERIFY(!QFile::exists(path) || QStandardPaths::isTestModeEnabled());
}

QTEST_GUILESS_MAIN(Tst_sshprofile)
#include "tst_sshprofile.moc"
