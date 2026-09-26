#include <QtTest>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "ssh/SecretStore.h"

/// SecretStore round trips against an isolated INI QSettings file (never the registry or the
/// real configuration). The Windows backend goes through DPAPI, the others through the XOR
/// obfuscation; the observable contract is the same.
class Tst_secretstore : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void roundTrip();
    void overwriteReplaces();
    void emptySecretRemoves();
    void keysContainingSlashes();
    void unicodeSecret();
    void absentKey();
    void tamperedValue();
    void valueIsNotStoredInClear();
    void emptyKeyRejected();
    void isSecureMatchesPlatform();
    void storageDescriptionNotEmpty();

private:
    static QStringList rawSecretKeys();
    QTemporaryDir m_dir;
};

void Tst_secretstore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-secretstore"));
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().startsWith(m_dir.path()));
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-secretstore")));
}

void Tst_secretstore::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

QStringList Tst_secretstore::rawSecretKeys()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("secrets"));
    QStringList keys = settings.childKeys();
    settings.endGroup();
    keys.removeAll(QStringLiteral("_key"));
    return keys;
}

void Tst_secretstore::roundTrip()
{
    const QString key = QStringLiteral("ssh/abc-123/password");
    QVERIFY(!SecretStore::contains(key));
    QVERIFY(SecretStore::store(key, QStringLiteral("hunter2")));
    QVERIFY(SecretStore::contains(key));
    const std::optional<QString> loaded = SecretStore::load(key);
    QVERIFY(loaded.has_value());
    QCOMPARE(*loaded, QStringLiteral("hunter2"));

    // A second QSettings instance (fresh read from disk) sees it as well.
    QSettings settings;
    settings.sync();
    QCOMPARE(rawSecretKeys().size(), 1);

    QVERIFY(SecretStore::remove(key));
    QVERIFY(!SecretStore::contains(key));
    QVERIFY(!SecretStore::load(key).has_value());
    QVERIFY(!SecretStore::remove(key));
    QVERIFY(rawSecretKeys().isEmpty());
}

void Tst_secretstore::overwriteReplaces()
{
    const QString key = QStringLiteral("ssh/abc/password");
    QVERIFY(SecretStore::store(key, QStringLiteral("first")));
    QVERIFY(SecretStore::store(key, QStringLiteral("second")));
    QCOMPARE(SecretStore::load(key).value_or(QString()), QStringLiteral("second"));
    QCOMPARE(rawSecretKeys().size(), 1);
}

void Tst_secretstore::emptySecretRemoves()
{
    const QString key = QStringLiteral("ssh/abc/passphrase");
    QVERIFY(SecretStore::store(key, QStringLiteral("something")));
    QVERIFY(SecretStore::contains(key));
    QVERIFY(SecretStore::store(key, QString()));
    QVERIFY(!SecretStore::contains(key));
    QVERIFY(!SecretStore::load(key).has_value());
    // Removing what is not there reports false; storing "" for an absent key is the same.
    QVERIFY(!SecretStore::store(key, QString()));
}

void Tst_secretstore::keysContainingSlashes()
{
    const QString password = QStringLiteral("ssh/4f2c/password");
    const QString passphrase = QStringLiteral("ssh/4f2c/passphrase");
    const QString other = QStringLiteral("ssh/4f2c-2/password");
    QVERIFY(SecretStore::store(password, QStringLiteral("pw")));
    QVERIFY(SecretStore::store(passphrase, QStringLiteral("pp")));
    QVERIFY(SecretStore::store(other, QStringLiteral("other")));
    QCOMPARE(SecretStore::load(password).value_or(QString()), QStringLiteral("pw"));
    QCOMPARE(SecretStore::load(passphrase).value_or(QString()), QStringLiteral("pp"));
    QCOMPARE(SecretStore::load(other).value_or(QString()), QStringLiteral("other"));

    // "/" never became a QSettings group: everything sits flat under "secrets".
    QCOMPARE(rawSecretKeys().size(), 3);
    QSettings settings;
    QVERIFY(!settings.childGroups().contains(QStringLiteral("ssh")));
    settings.beginGroup(QStringLiteral("secrets"));
    QVERIFY(settings.childGroups().isEmpty());
    settings.endGroup();

    QVERIFY(SecretStore::remove(passphrase));
    QCOMPARE(SecretStore::load(password).value_or(QString()), QStringLiteral("pw"));
    QVERIFY(!SecretStore::contains(passphrase));
}

void Tst_secretstore::unicodeSecret()
{
    const QString key = QStringLiteral("ssh/unicode/password");
    const QString secret = QString::fromUtf8("p\xC3\xA4ss \xE5\xAF\x86\xE7\xA0\x81 \xF0\x9F\x94\x91 caf\xC3\xA9");
    QVERIFY(SecretStore::store(key, secret));
    QCOMPARE(SecretStore::load(key).value_or(QString()), secret);

    const QString longSecret = QString(5000, QChar(0x00E9)) + QStringLiteral("end");
    QVERIFY(SecretStore::store(key, longSecret));
    QCOMPARE(SecretStore::load(key).value_or(QString()), longSecret);
}

void Tst_secretstore::absentKey()
{
    QVERIFY(!SecretStore::load(QStringLiteral("ssh/never-stored/password")).has_value());
    QVERIFY(!SecretStore::contains(QStringLiteral("ssh/never-stored/password")));
    QVERIFY(!SecretStore::remove(QStringLiteral("ssh/never-stored/password")));
}

void Tst_secretstore::tamperedValue()
{
    const QString key = QStringLiteral("ssh/tamper/password");
    const QString original = QStringLiteral("correct horse battery staple");
    QVERIFY(SecretStore::store(key, original));
    const QStringList raw = rawSecretKeys();
    QCOMPARE(raw.size(), 1);
    const QString entry = QStringLiteral("secrets/") + raw.first();

    // Garbage that is not the stored blob: never the original, never a crash.
    {
        QSettings settings;
        settings.setValue(entry, QStringLiteral("!!!not-base64!!!"));
        settings.sync();
    }
    QVERIFY(SecretStore::contains(key));
    std::optional<QString> loaded = SecretStore::load(key);
    QVERIFY(!loaded.has_value() || *loaded != original);

    {
        QSettings settings;
        settings.setValue(entry, QString::fromLatin1(QByteArray("definitely not a DPAPI blob").toBase64()));
        settings.sync();
    }
    loaded = SecretStore::load(key);
    QVERIFY(!loaded.has_value() || *loaded != original);

    // An empty stored value is "absent".
    {
        QSettings settings;
        settings.setValue(entry, QString());
        settings.sync();
    }
    QVERIFY(!SecretStore::load(key).has_value());

    // Storing again repairs it.
    QVERIFY(SecretStore::store(key, original));
    QCOMPARE(SecretStore::load(key).value_or(QString()), original);
}

void Tst_secretstore::valueIsNotStoredInClear()
{
    const QString key = QStringLiteral("ssh/clear/password");
    const QString secret = QStringLiteral("VerySecretValue-9876");
    QVERIFY(SecretStore::store(key, secret));
    QSettings settings;
    settings.sync();
    QFile file(settings.fileName());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray content = file.readAll();
    QVERIFY(!content.contains(secret.toUtf8()));
    QVERIFY(!content.contains(secret.toUtf8().toBase64()));
    QVERIFY(!content.contains(key.toUtf8()));   // the key is encoded as well
}

void Tst_secretstore::emptyKeyRejected()
{
    QVERIFY(!SecretStore::store(QString(), QStringLiteral("x")));
    QVERIFY(!SecretStore::load(QString()).has_value());
    QVERIFY(!SecretStore::contains(QString()));
    QVERIFY(!SecretStore::remove(QString()));
    QVERIFY(rawSecretKeys().isEmpty());
}

void Tst_secretstore::isSecureMatchesPlatform()
{
#if defined(Q_OS_WIN)
    QVERIFY(SecretStore::isSecure());
#else
    QVERIFY(!SecretStore::isSecure());
#endif
}

void Tst_secretstore::storageDescriptionNotEmpty()
{
    const QString description = SecretStore::storageDescription();
    QVERIFY(!description.isEmpty());
#if defined(Q_OS_WIN)
    QVERIFY(description.contains(QStringLiteral("DPAPI")));
#else
    QVERIFY(description.contains(QStringLiteral("not encrypted")));
#endif
}

QTEST_GUILESS_MAIN(Tst_secretstore)
#include "tst_secretstore.moc"
