#include <QtTest>
#include <QSignalSpy>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>

#include <memory>

#include "firmware/ImagePuller.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "support/TestSshServer.h"

/*
 * ImagePuller against the in-process TestSshServer (SFTP and, with Options::allowSftp = false, the
 * shell fallback): the full pull into an empty cache and its note, the skip rule (size, mtime,
 * file present), forceDownload, one-level history and roll back, the RKFW check, missing files,
 * `git describe`, extra files, cancel (before and during the transfer), a dropped connection, the
 * start() prerequisites, the folder spellings ("~", absolute, trailing slash) and the cache notes.
 * The server's files live in a temporary directory (the remote home); nothing touches a real
 * machine. SshConnection::runCommand() / statRemote() are tested in tst_sshconnection.
 */

namespace {

constexpr int kTimeoutMs = 30000;
constexpr int kBigImage = 16 * 1024 * 1024;   ///< big enough to still be in flight when a slot reacts to the first progress

const QString kRemoteFolder = QStringLiteral("~/rk/sdk/output/image");
const QString kRemoteImage = QStringLiteral("rk/sdk/output/image/update.img");   ///< relative to the server's root
const QString kGit = QStringLiteral("v1.3.0-4-ge0e0d41");
const QDateTime kStampA(QDate(2026, 10, 13), QTime(13, 45, 50), QTimeZone::UTC);
const QDateTime kStampB(QDate(2026, 10, 14), QTime(8, 1, 2), QTimeZone::UTC);
const QDateTime kStampC(QDate(2026, 10, 15), QTime(23, 59, 58), QTimeZone::UTC);

/// "RKFW" followed by `fill` bytes: what the puller accepts as an update package.
QByteArray rkfwImage(qsizetype size, char fill)
{
    QByteArray bytes(size, fill);
    bytes.replace(0, 4, QByteArrayLiteral("RKFW"));
    return bytes;
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool setModified(const QString& path, const QDateTime& when)
{
    QFile file(path);
    return file.open(QIODevice::ReadWrite) && file.setFileTime(when, QFileDevice::FileModificationTime);
}

QString sizeText(qint64 bytes)
{
    return QLocale::c().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

QString timeText(const QDateTime& when)
{
    return when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

/// The server's signals come from its own threads; this object lives in the test thread, so the
/// connections are queued and the lists are only ever touched here (poll them with QTRY_*).
class ServerCounters : public QObject
{
public:
    explicit ServerCounters(TestSshServer* server)
    {
        connect(server, &TestSshServer::execRequested, this, [this](const QString& command) { execCommands.append(command); });
        connect(server, &TestSshServer::sftpRequested, this, [this] { ++sftp; });
    }

    QStringList execCommands;
    int sftp = 0;
};

/// Every signal of one ImagePuller, recorded.
struct Spies
{
    explicit Spies(ImagePuller* puller)
        : finished(puller, &ImagePuller::finished)
        , info(puller, &ImagePuller::remoteInfo)
        , progress(puller, &ImagePuller::progress)
        , skipped(puller, &ImagePuller::skipped)
        , log(puller, &ImagePuller::logLine)
    {
    }

    bool ok(int run) const { return finished.at(run).at(0).toBool(); }
    QString message(int run) const { return finished.at(run).at(1).toString(); }
    bool logContains(const QString& text) const
    {
        for (const QList<QVariant>& args : log) {
            if (args.at(0).toString().contains(text)) {
                return true;
            }
        }
        return false;
    }

    QSignalSpy finished;
    QSignalSpy info;
    QSignalSpy progress;
    QSignalSpy skipped;
    QSignalSpy log;
};

} // namespace

class Tst_imagepuller : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();

    void pullIntoEmptyCache();
    void unchangedIsSkipped();
    void skipRuleChecksSizeTimeAndFile();
    void forceDownloadKeepsHistory();
    void changedImageRotatesHistory();
    void rollBackTwice();
    void rollBackEdgeCases();
    void wrongMagicKeepsCurrentImage();
    void missingRemoteFile();
    void gitDescribeShownAndAbsent();
    void extraFiles();
    void cancelMidTransfer_data();
    void cancelMidTransfer();
    void cancelBeforeTransfer();
    void destroyWhileRunning();
    void connectionDropped();
    void connectionClosedDuringStat();
    void startPrerequisitesAndBusy();
    void remoteFolderForms();
    void shellFallbackPull();
    void cacheNotes();

private:
    void startRig(TestSshServer::Options options = TestSshServer::Options());   ///< server + connected connection + puller
    void stopRig();
    void pullAndWait(Spies& spies);                                               ///< start() and wait for the next finished()
    bool putRemote(const QString& relative, const QByteArray& payload, const QDateTime& stamp);
    QString local(const QString& name = QStringLiteral("update.img")) const { return QDir(m_cache).filePath(name); }

    QTemporaryDir m_dir;
    int m_testNumber = 0;
    QString m_work;
    QString m_root;     ///< the server's root = the remote home
    QString m_cache;
    std::unique_ptr<TestSshServer> m_server;
    std::unique_ptr<ServerCounters> m_counters;
    std::unique_ptr<SshConnection> m_conn;
    std::unique_ptr<ImagePuller> m_puller;
};

#define RIG(...)                                                                                   \
    do {                                                                                           \
        startRig(__VA_ARGS__);                                                                     \
        if (QTest::currentTestFailed()) {                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (false)

#define PULL(spies)                                                                                \
    do {                                                                                           \
        pullAndWait(spies);                                                                        \
        if (QTest::currentTestFailed()) {                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (false)

void Tst_imagepuller::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-imagepuller"));
    QVERIFY(m_dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    // Never read the developer's ~/.ssh/config (Host blocks, ProxyJump...).
    qputenv("SU_SSH_IGNORE_CONFIG", "1");
}

void Tst_imagepuller::init()
{
    m_work = m_dir.filePath(QString::number(++m_testNumber));
    m_root = m_work + QStringLiteral("/root");
    m_cache = m_work + QStringLiteral("/cache");   // not created: the puller makes it
    QVERIFY(QDir().mkpath(m_root));
}

void Tst_imagepuller::cleanup()
{
    stopRig();
}

void Tst_imagepuller::stopRig()
{
    m_puller.reset();
    m_conn.reset();
    m_counters.reset();
    m_server.reset();
}

void Tst_imagepuller::startRig(TestSshServer::Options options)
{
    stopRig();
    options.rootDir = m_root;
    m_server = std::make_unique<TestSshServer>(options);
    QVERIFY(m_server->start());
    m_counters = std::make_unique<ServerCounters>(m_server.get());

    m_conn = std::make_unique<SshConnection>();
    SshProfile profile;
    profile.host = QStringLiteral("127.0.0.1");
    profile.port = m_server->port();
    profile.user = QStringLiteral("test");
    profile.auth = SshProfile::Auth::Password;   // never Auto: that would read the real ~/.ssh/id_* keys
    profile.keepAliveSeconds = 0;
    profile.connectTimeoutSeconds = 5;
    profile.knownHostsFile = m_work + QStringLiteral("/known_hosts");
    m_conn->setProfile(profile);
    m_conn->setAutoReconnect(false);
    SshConnection* conn = m_conn.get();
    QObject::connect(conn, &SshConnection::hostKeyVerificationRequired, conn, [conn] { conn->answerHostKey(true, true); });
    QObject::connect(conn, &SshConnection::authPromptRequired, conn,
                     [conn] { conn->answerPrompt(QStringLiteral("secret"), false); });
    QVERIFY(conn->open());
    QTRY_COMPARE_WITH_TIMEOUT(conn->state(), Transport::State::Connected, kTimeoutMs);

    m_puller = std::make_unique<ImagePuller>();
    m_puller->setConnection(conn);
    m_puller->setRemoteFolder(kRemoteFolder);
    m_puller->setCacheDirectory(m_cache);
}

void Tst_imagepuller::pullAndWait(Spies& spies)
{
    const int before = static_cast<int>(spies.finished.count());
    QVERIFY(m_puller->start());
    QVERIFY(m_puller->isRunning());
    QTRY_COMPARE_WITH_TIMEOUT(static_cast<int>(spies.finished.count()), before + 1, kTimeoutMs);
    QVERIFY(!m_puller->isRunning());
}

bool Tst_imagepuller::putRemote(const QString& relative, const QByteArray& payload, const QDateTime& stamp)
{
    const QString path = QDir(m_root).filePath(relative);
    return writeFile(path, payload) && setModified(path, stamp);
}

// ---------------------------------------------------------------------------------------

void Tst_imagepuller::pullIntoEmptyCache()
{
    TestSshServer::Options options;
    options.gitDescribeOutput = kGit;
    RIG(options);
    const QByteArray image = rkfwImage(300 * 1024, 'a');
    QVERIFY(putRemote(kRemoteImage, image, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    QVERIFY(!QFileInfo::exists(m_cache));
    QCOMPARE(m_puller->localImagePath(), QString());

    PULL(s);
    QCOMPARE(s.finished.count(), 1);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));
    const QString target = m_conn->profile().displayTarget();
    QCOMPARE(s.message(0), QStringLiteral("pulled update.img (%1) from %2:%3").arg(sizeText(image.size()), target, kRemoteFolder));

    // What the build machine said: name, size, UTC mtime, the git version.
    QCOMPARE(s.info.count(), 1);
    QCOMPARE(s.info.at(0).at(0).toString(), QStringLiteral("update.img"));
    QCOMPARE(s.info.at(0).at(1).toLongLong(), qint64(image.size()));
    QCOMPARE(s.info.at(0).at(2).toDateTime().toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(s.info.at(0).at(3).toString(), kGit);
    QCOMPARE(s.skipped.count(), 0);
    QCOMPARE(started.count(), 1);

    // Progress ends at the full size.
    QVERIFY(s.progress.count() >= 1);
    QCOMPARE(s.progress.last().at(0).toLongLong(), qint64(image.size()));
    QCOMPARE(s.progress.last().at(1).toLongLong(), qint64(image.size()));

    // The image, no leftovers, no history yet.
    QCOMPARE(m_puller->localImagePath(), ImagePuller::imagePath(m_cache));
    QVERIFY2(readAll(local()) == image, "cached image differs from the remote one");
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new.part"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.part"))));
    QVERIFY(!QFileInfo::exists(ImagePuller::previousImagePath(m_cache)));
    QVERIFY(!ImagePuller::previousImage(m_cache).isValid());

    // The note next to it.
    QVERIFY(QFileInfo::exists(local(QStringLiteral("update.img.json"))));
    const ImagePuller::CacheEntry entry = ImagePuller::cachedImage(m_cache);
    QVERIFY(entry.isValid());
    QCOMPARE(entry.path, ImagePuller::imagePath(m_cache));
    QCOMPARE(entry.size, qint64(image.size()));
    QCOMPARE(entry.remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(entry.target, target);
    QCOMPARE(entry.remoteFolder, kRemoteFolder);
    QCOMPARE(entry.gitDescribe, kGit);
    QVERIFY(entry.pulledAt.isValid());
    QVERIFY(qAbs(entry.pulledAt.secsTo(QDateTime::currentDateTimeUtc())) < 120);
    const QString summary = entry.summary();
    QVERIFY2(summary.startsWith(QStringLiteral("update.img %1, built %2 (%3), pulled ").arg(sizeText(image.size()), timeText(kStampA), kGit)),
             qPrintable(summary));
    QVERIFY2(summary.endsWith(QStringLiteral(" from %1").arg(target)), qPrintable(summary));

    // The status the tab shows while it works.
    QVERIFY(s.logContains(QStringLiteral("Downloading update.img")));
    QVERIFY(s.logContains(kGit));
}

void Tst_imagepuller::unchangedIsSkipped()
{
    TestSshServer::Options options;
    options.gitDescribeOutput = kGit;
    RIG(options);
    const QByteArray image = rkfwImage(200 * 1024, 'a');
    QVERIFY(putRemote(kRemoteImage, image, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    PULL(s);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));
    QCOMPARE(started.count(), 1);
    const ImagePuller::CacheEntry first = ImagePuller::cachedImage(m_cache);
    const QDateTime localTime = QFileInfo(local()).lastModified();

    PULL(s);
    QCOMPARE(s.finished.count(), 2);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    const QString expected = QStringLiteral("update.img unchanged (%1, %2)").arg(sizeText(image.size()), timeText(kStampA));
    QCOMPARE(s.message(1), expected);
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(s.skipped.at(0).at(0).toString(), expected);
    QCOMPARE(started.count(), 1);                    // no transfer
    QCOMPARE(s.info.count(), 2);                     // the remote facts are reported every time
    QCOMPARE(s.info.at(1).at(1).toLongLong(), qint64(image.size()));
    QCOMPARE(m_puller->localImagePath(), ImagePuller::imagePath(m_cache));
    QVERIFY2(readAll(local()) == image, "cached image changed by a skipped pull");
    QCOMPARE(QFileInfo(local()).lastModified(), localTime);
    QVERIFY(!QFileInfo::exists(ImagePuller::previousImagePath(m_cache)));
    QCOMPARE(ImagePuller::cachedImage(m_cache).pulledAt, first.pulledAt);   // note untouched
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
}

void Tst_imagepuller::skipRuleChecksSizeTimeAndFile()
{
    RIG();
    const QByteArray image = rkfwImage(150 * 1024, 'a');
    QVERIFY(putRemote(kRemoteImage, image, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    PULL(s);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));
    QCOMPARE(started.count(), 1);

    // The cached file is gone (the note alone is no proof): pulled again.
    QVERIFY(QFile::remove(local()));
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QCOMPARE(s.skipped.count(), 0);
    QCOMPARE(started.count(), 2);
    QVERIFY2(readAll(local()) == image, "restored image differs");

    // Same size, newer mtime on the build machine: a new build, pulled and rotated.
    QVERIFY(setModified(QDir(m_root).filePath(kRemoteImage), kStampB));
    PULL(s);
    QVERIFY2(s.ok(2), qPrintable(s.message(2)));
    QCOMPARE(s.skipped.count(), 0);
    QCOMPARE(started.count(), 3);
    QVERIFY(ImagePuller::previousImage(m_cache).isValid());
    QCOMPARE(ImagePuller::previousImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());

    // A cached file that no longer has the size the note says (replaced by hand): pulled again.
    QVERIFY(writeFile(local(), rkfwImage(1000, 'z')));
    PULL(s);
    QVERIFY2(s.ok(3), qPrintable(s.message(3)));
    QCOMPARE(s.skipped.count(), 0);
    QCOMPARE(started.count(), 4);
    QVERIFY2(readAll(local()) == image, "tampered image was not replaced");

    // And with everything in order again: skipped.
    PULL(s);
    QVERIFY2(s.ok(4), qPrintable(s.message(4)));
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(started.count(), 4);
}

void Tst_imagepuller::forceDownloadKeepsHistory()
{
    RIG();
    const QByteArray a = rkfwImage(120 * 1024, 'a');
    const QByteArray b = rkfwImage(140 * 1024, 'b');
    QVERIFY(putRemote(kRemoteImage, a, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    PULL(s);
    QVERIFY(putRemote(kRemoteImage, b, kStampB));
    PULL(s);
    QVERIFY2(s.ok(0) && s.ok(1), qPrintable(s.message(1)));
    QCOMPARE(started.count(), 2);
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == a, "previous image is not the first build");

    // forceDownload overrides the skip rule ...
    QVERIFY(!m_puller->forceDownload());
    m_puller->setForceDownload(true);
    QVERIFY(m_puller->forceDownload());
    const QDateTime pulledBefore = ImagePuller::cachedImage(m_cache).pulledAt;
    QTest::qWait(1100);   // the note keeps seconds
    PULL(s);
    QVERIFY2(s.ok(2), qPrintable(s.message(2)));
    QCOMPARE(s.skipped.count(), 0);
    QCOMPARE(started.count(), 3);
    QVERIFY2(readAll(local()) == b, "current image changed");
    QVERIFY(ImagePuller::cachedImage(m_cache).pulledAt > pulledBefore);
    // ... but a copy of the same build must not push the real previous image out of the history.
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == a, "forced re-pull of the same build rotated the history");
    QCOMPARE(ImagePuller::previousImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());

    // Back to the skip rule.
    m_puller->setForceDownload(false);
    PULL(s);
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(started.count(), 3);
}

void Tst_imagepuller::changedImageRotatesHistory()
{
    TestSshServer::Options options;
    options.gitDescribeOutput = kGit;
    RIG(options);
    const QByteArray v1 = rkfwImage(200 * 1024, 'a');
    const QByteArray v2 = rkfwImage(250 * 1024, 'b');
    const QByteArray v3 = rkfwImage(220 * 1024, 'c');
    Spies s(m_puller.get());

    QVERIFY(putRemote(kRemoteImage, v1, kStampA));
    PULL(s);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));

    // A new build: the old one becomes update.prev.img, with its note.
    QVERIFY(putRemote(kRemoteImage, v2, kStampB));
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QVERIFY2(readAll(local()) == v2, "current is not the new build");
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == v1, "previous is not the old build");
    const ImagePuller::CacheEntry previous = ImagePuller::previousImage(m_cache);
    QVERIFY(previous.isValid());
    QCOMPARE(previous.path, ImagePuller::previousImagePath(m_cache));
    QCOMPARE(previous.size, qint64(v1.size()));
    QCOMPARE(previous.remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(previous.gitDescribe, kGit);
    QVERIFY(QFileInfo::exists(local(QStringLiteral("update.prev.img.json"))));
    const ImagePuller::CacheEntry current = ImagePuller::cachedImage(m_cache);
    QCOMPARE(current.size, qint64(v2.size()));
    QCOMPARE(current.remoteModified.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());
    QVERIFY(previous.summary().startsWith(QStringLiteral("update.prev.img ")));

    // A third one: one level of history only, the first build is gone.
    QVERIFY(putRemote(kRemoteImage, v3, kStampC));
    PULL(s);
    QVERIFY2(s.ok(2), qPrintable(s.message(2)));
    QVERIFY2(readAll(local()) == v3, "current is not the third build");
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == v2, "previous is not the second build");
    QCOMPARE(ImagePuller::previousImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampC.toSecsSinceEpoch());
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.replaced"))));
    QCOMPARE(QDir(m_cache).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name),
             QStringList({QStringLiteral("update.img"), QStringLiteral("update.img.json"), QStringLiteral("update.prev.img"),
                          QStringLiteral("update.prev.img.json")}));
}

void Tst_imagepuller::rollBackTwice()
{
    RIG();
    const QByteArray v1 = rkfwImage(200 * 1024, 'a');
    const QByteArray v2 = rkfwImage(250 * 1024, 'b');
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    QVERIFY(putRemote(kRemoteImage, v1, kStampA));
    PULL(s);
    QVERIFY(putRemote(kRemoteImage, v2, kStampB));
    PULL(s);
    QVERIFY2(s.ok(0) && s.ok(1), qPrintable(s.message(1)));

    QString error;
    QVERIFY2(ImagePuller::rollBack(m_cache, &error), qPrintable(error));
    QVERIFY2(readAll(local()) == v1, "roll back did not restore the old build");
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == v2, "roll back lost the newer build");
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(ImagePuller::cachedImage(m_cache).size, qint64(v1.size()));
    QCOMPARE(ImagePuller::previousImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.swap"))));

    // Again: back where we started - and that is the build the server has, so the next pull skips.
    QVERIFY2(ImagePuller::rollBack(m_cache, &error), qPrintable(error));
    QVERIFY2(readAll(local()) == v2, "second roll back did not restore the newer build");
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == v1, "second roll back lost the old build");
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());
    QCOMPARE(ImagePuller::previousImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    PULL(s);
    QVERIFY2(s.ok(2), qPrintable(s.message(2)));
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(started.count(), 2);

    // Rolled back to the old build, the server's newer one is pulled again and the history moves on.
    QVERIFY2(ImagePuller::rollBack(m_cache, &error), qPrintable(error));
    PULL(s);
    QVERIFY2(s.ok(3), qPrintable(s.message(3)));
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(started.count(), 3);
    QVERIFY2(readAll(local()) == v2, "pull after a roll back did not fetch the server's build");
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == v1, "the rolled-back build is not the previous one");
}

void Tst_imagepuller::rollBackEdgeCases()
{
    QString error;
    const QString cache = m_work + QStringLiteral("/edge");
    QVERIFY(QDir().mkpath(cache));

    // Nothing at all, and an image without a previous one.
    QVERIFY(!ImagePuller::rollBack(cache, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(writeFile(ImagePuller::imagePath(cache), rkfwImage(100, 'a')));
    error.clear();
    QVERIFY(!ImagePuller::rollBack(cache, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!ImagePuller::rollBack(cache));   // the error text is optional
    QVERIFY2(readAll(ImagePuller::imagePath(cache)) == rkfwImage(100, 'a'), "a refused roll back touched the image");

    // A previous image without a current one (deleted by hand): it just moves back.
    QVERIFY(QFile::remove(ImagePuller::imagePath(cache)));
    QVERIFY(writeFile(ImagePuller::previousImagePath(cache), rkfwImage(100, 'b')));
    QVERIFY2(ImagePuller::rollBack(cache, &error), qPrintable(error));
    QVERIFY2(readAll(ImagePuller::imagePath(cache)) == rkfwImage(100, 'b'), "previous image did not come back");
    QVERIFY(!QFileInfo::exists(ImagePuller::previousImagePath(cache)));

    // Images without notes swap like any other pair.
    QVERIFY(writeFile(ImagePuller::previousImagePath(cache), rkfwImage(100, 'c')));
    QVERIFY2(ImagePuller::rollBack(cache, &error), qPrintable(error));
    QVERIFY2(readAll(ImagePuller::imagePath(cache)) == rkfwImage(100, 'c'), "image without a note did not swap");
    QVERIFY2(readAll(ImagePuller::previousImagePath(cache)) == rkfwImage(100, 'b'), "previous image without a note did not swap");
}

void Tst_imagepuller::wrongMagicKeepsCurrentImage()
{
    RIG();
    Spies s(m_puller.get());

    // An empty cache and a file that is not an RKFW package: nothing is created.
    QVERIFY(putRemote(kRemoteImage, QByteArray("XXXX") + QByteArray(100 * 1024, 'x'), kStampA));
    PULL(s);
    QCOMPARE(s.finished.count(), 1);
    QVERIFY(!s.ok(0));
    QCOMPARE(s.message(0), QStringLiteral("downloaded file is not a Rockchip RKFW image"));
    QVERIFY(!QFileInfo::exists(local()));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.json"))));
    QCOMPARE(m_puller->localImagePath(), QString());

    // A good image first; a later bad build must not replace it, not even into the history.
    const QByteArray good = rkfwImage(180 * 1024, 'g');
    QVERIFY(putRemote(kRemoteImage, good, kStampB));
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QVERIFY(putRemote(kRemoteImage, QByteArray("RKF") + QByteArray(90 * 1024, 'x'), kStampC));   // one byte short of the magic
    PULL(s);
    QVERIFY(!s.ok(2));
    QVERIFY2(s.message(2).contains(QStringLiteral("RKFW")), qPrintable(s.message(2)));
    QVERIFY2(readAll(local()) == good, "a bad download replaced the good image");
    QVERIFY(!QFileInfo::exists(ImagePuller::previousImagePath(m_cache)));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());   // the note still describes the good one
    QCOMPARE(m_puller->localImagePath(), QString());
    QVERIFY(!m_puller->isRunning());

    // A file shorter than the magic fails the same way.
    QVERIFY(putRemote(kRemoteImage, QByteArray("RK"), kStampA));
    PULL(s);
    QVERIFY(!s.ok(3));
    QVERIFY2(readAll(local()) == good, "a two-byte download replaced the good image");
}

void Tst_imagepuller::missingRemoteFile()
{
    RIG();
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    const QString target = m_conn->profile().displayTarget();

    // No file in an existing folder.
    QVERIFY(QDir().mkpath(QDir(m_root).filePath(QStringLiteral("rk/sdk/output/image"))));
    PULL(s);
    QVERIFY(!s.ok(0));
    QCOMPARE(s.message(0), QStringLiteral("no update.img in %1 on %2").arg(kRemoteFolder, target));
    QCOMPARE(s.info.count(), 0);
    QCOMPARE(started.count(), 0);
    QCOMPARE(m_puller->localImagePath(), QString());
    QVERIFY(!QFileInfo::exists(local()));

    // No folder at all.
    m_puller->setRemoteFolder(QStringLiteral("~/nowhere/output/image"));
    PULL(s);
    QVERIFY(!s.ok(1));
    QCOMPARE(s.message(1), QStringLiteral("no update.img in ~/nowhere/output/image on %1").arg(target));

    // update.img is a folder.
    QVERIFY(QDir().mkpath(QDir(m_root).filePath(QStringLiteral("rk/sdk/output/image/update.img"))));
    m_puller->setRemoteFolder(kRemoteFolder);
    PULL(s);
    QVERIFY(!s.ok(2));
    QVERIFY2(s.message(2).contains(QStringLiteral("folder")), qPrintable(s.message(2)));
    QCOMPARE(started.count(), 0);
    QVERIFY(!QFileInfo::exists(local()));
}

void Tst_imagepuller::gitDescribeShownAndAbsent()
{
    const QByteArray image = rkfwImage(100 * 1024, 'a');
    const QString defaultCommand = QStringLiteral("git -C 'rk/sdk' describe --tags --dirty 2>/dev/null");

    // 1. Shown: the grandparent of the folder is the SDK; the version goes to remoteInfo and the note.
    {
        TestSshServer::Options options;
        options.gitDescribeOutput = kGit;
        RIG(options);
        QVERIFY(putRemote(kRemoteImage, image, kStampA));
        Spies s(m_puller.get());
        QVERIFY(m_puller->gitDescribe());
        PULL(s);
        QVERIFY2(s.ok(0), qPrintable(s.message(0)));
        QCOMPARE(s.info.at(0).at(3).toString(), kGit);
        QCOMPARE(ImagePuller::cachedImage(m_cache).gitDescribe, kGit);
        QTRY_VERIFY_WITH_TIMEOUT(m_counters->execCommands.contains(defaultCommand), kTimeoutMs);
        QCOMPARE(m_puller->sdkDirectory(), QString());
    }

    // 2. Absent (git fails, exit 128): not an error; the pull goes on without a version.
    {
        QDir(m_cache).removeRecursively();
        RIG();
        Spies s(m_puller.get());
        PULL(s);
        QVERIFY2(s.ok(0), qPrintable(s.message(0)));
        QCOMPARE(s.info.at(0).at(3).toString(), QString());
        QCOMPARE(ImagePuller::cachedImage(m_cache).gitDescribe, QString());
        QVERIFY(s.logContains(QStringLiteral("No git version")));
        QTRY_VERIFY_WITH_TIMEOUT(m_counters->execCommands.contains(defaultCommand), kTimeoutMs);
    }

    // 3. Switched off: git is not even asked.
    {
        QDir(m_cache).removeRecursively();
        TestSshServer::Options options;
        options.gitDescribeOutput = kGit;
        RIG(options);
        m_puller->setGitDescribe(false);
        QVERIFY(!m_puller->gitDescribe());
        Spies s(m_puller.get());
        PULL(s);
        QVERIFY2(s.ok(0), qPrintable(s.message(0)));
        QCOMPARE(s.info.at(0).at(3).toString(), QString());
        QCOMPARE(s.info.at(0).at(1).toLongLong(), qint64(image.size()));
        // stat and the download are SFTP here; any exec command at all would be the git one
        const int barrier = m_conn->runCommand(QStringLiteral("echo barrier"));
        QVERIFY(barrier > 0);
        QTRY_VERIFY_WITH_TIMEOUT(m_counters->execCommands.contains(QStringLiteral("echo barrier")), kTimeoutMs);
        QCOMPARE(m_counters->execCommands, QStringList{QStringLiteral("echo barrier")});
    }

    // 4. An explicit SDK folder wins over the derived one (and "~/" is turned into a relative path).
    {
        QDir(m_cache).removeRecursively();
        TestSshServer::Options options;
        options.gitDescribeOutput = kGit;
        RIG(options);
        m_puller->setSdkDirectory(QStringLiteral("~/rk"));
        QCOMPARE(m_puller->sdkDirectory(), QStringLiteral("~/rk"));
        Spies s(m_puller.get());
        PULL(s);
        QVERIFY2(s.ok(0), qPrintable(s.message(0)));
        QCOMPARE(s.info.at(0).at(3).toString(), kGit);
        QTRY_VERIFY_WITH_TIMEOUT(m_counters->execCommands.contains(QStringLiteral("git -C 'rk' describe --tags --dirty 2>/dev/null")),
                                 kTimeoutMs);
    }

    // 5. A folder name with a quote reaches git as one word; a folder that is not there means no version.
    {
        QDir(m_cache).removeRecursively();
        TestSshServer::Options options;
        options.gitDescribeOutput = kGit;
        RIG(options);
        QVERIFY(QDir().mkpath(QDir(m_root).filePath(QStringLiteral("it's here"))));
        m_puller->setSdkDirectory(QStringLiteral("~/it's here"));
        Spies s(m_puller.get());
        PULL(s);
        QVERIFY2(s.ok(0), qPrintable(s.message(0)));
        QCOMPARE(s.info.at(0).at(3).toString(), kGit);
        QTRY_VERIFY_WITH_TIMEOUT(
            m_counters->execCommands.contains(QStringLiteral("git -C 'it'\\''s here' describe --tags --dirty 2>/dev/null")), kTimeoutMs);
        m_puller->setSdkDirectory(QStringLiteral("~/nowhere"));
        PULL(s);
        QVERIFY2(s.ok(1), qPrintable(s.message(1)));
        QCOMPARE(s.info.at(1).at(3).toString(), QString());
    }
}

void Tst_imagepuller::extraFiles()
{
    RIG();
    const QByteArray image = rkfwImage(100 * 1024, 'a');
    QVERIFY(putRemote(kRemoteImage, image, kStampA));
    QVERIFY(putRemote(QStringLiteral("rk/sdk/output/image/sd_update.txt"), QByteArrayLiteral("sd update\n"), kStampA));
    QVERIFY(putRemote(QStringLiteral("rk/sdk/output/image/other.txt"), QByteArray(70 * 1024, 'o'), kStampA));
    QVERIFY(putRemote(QStringLiteral("rk/sdk/evil.txt"), QByteArrayLiteral("outside"), kStampA));
    QDir().mkpath(QDir(m_root).filePath(QStringLiteral("rk/sdk/output/image/folder.d")));

    m_puller->setExtraFiles({QStringLiteral("sd_update.txt"), QStringLiteral("missing.txt"), QStringLiteral("../evil.txt"),
                             QStringLiteral("sub/x.txt"), QStringLiteral("update.img"), QStringLiteral("folder.d"),
                             QStringLiteral("  other.txt "), QStringLiteral("sd_update.txt"), QString()});
    QCOMPARE(m_puller->extraFiles().size(), 9);
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    PULL(s);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));
    // The message is the image's, whatever happened to the extras.
    QVERIFY2(s.message(0).startsWith(QStringLiteral("pulled update.img (")), qPrintable(s.message(0)));
    QCOMPARE(started.count(), 3);   // the image, sd_update.txt, other.txt
    QCOMPARE(readAll(local(QStringLiteral("sd_update.txt"))), QByteArrayLiteral("sd update\n"));
    QVERIFY2(readAll(local(QStringLiteral("other.txt"))) == QByteArray(70 * 1024, 'o'), "other.txt differs");
    QVERIFY2(readAll(local()) == image, "image differs");
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("missing.txt"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("folder.d"))));
    QVERIFY(!QFileInfo::exists(m_work + QStringLiteral("/evil.txt")));        // nothing left the cache folder
    QVERIFY(!QFileInfo::exists(m_cache + QStringLiteral("/../evil.txt")));
    QCOMPARE(QDir(m_cache).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name),
             QStringList({QStringLiteral("other.txt"), QStringLiteral("sd_update.txt"), QStringLiteral("update.img"),
                          QStringLiteral("update.img.json")}));
    QVERIFY(s.logContains(QStringLiteral("Skipping missing.txt")));
    QVERIFY(s.logContains(QStringLiteral("Skipping ../evil.txt")));
    QVERIFY(s.logContains(QStringLiteral("Skipping sub/x.txt")));
    QVERIFY(s.logContains(QStringLiteral("Skipping update.img")));
    QVERIFY(s.logContains(QStringLiteral("Skipping folder.d")));
    QVERIFY(s.logContains(QStringLiteral("Pulled sd_update.txt")));
    QVERIFY(s.logContains(QStringLiteral("Pulled other.txt")));

    // Extras are overwritten by the next real pull ...
    QVERIFY(putRemote(QStringLiteral("rk/sdk/output/image/sd_update.txt"), QByteArrayLiteral("v2\n"), kStampB));
    QVERIFY(putRemote(kRemoteImage, rkfwImage(110 * 1024, 'b'), kStampB));
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QCOMPARE(readAll(local(QStringLiteral("sd_update.txt"))), QByteArrayLiteral("v2\n"));
    QCOMPARE(started.count(), 6);
    // ... and an unchanged image ends the run before any transfer, extras or not.
    PULL(s);
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(started.count(), 6);
}

void Tst_imagepuller::cancelMidTransfer_data()
{
    QTest::addColumn<bool>("sftp");
    QTest::newRow("sftp") << true;
    QTest::newRow("shell fallback") << false;
}

void Tst_imagepuller::cancelMidTransfer()
{
    QFETCH(bool, sftp);
    TestSshServer::Options options;
    options.allowSftp = sftp;
    RIG(options);
    const QByteArray big = rkfwImage(kBigImage, 'x');
    QVERIFY(putRemote(kRemoteImage, big, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);

    // Cancel from the first progress report: the transfer is in flight.
    bool cancelled = false;
    QObject::connect(m_puller.get(), &ImagePuller::progress, m_puller.get(), [&] {
        if (!cancelled) {
            cancelled = true;
            m_puller->cancel();
            m_puller->cancel();   // a second cancel changes nothing
        }
    });
    QVERIFY(m_puller->start());
    QTRY_COMPARE_WITH_TIMEOUT(s.finished.count(), 1, kTimeoutMs);
    QVERIFY(cancelled);
    QCOMPARE(started.count(), 1);
    QVERIFY(!s.ok(0));
    QCOMPARE(s.message(0), QStringLiteral("cancelled"));
    QVERIFY(!m_puller->isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(!m_conn->isTransferActive(), kTimeoutMs);
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new.part"))));
    QVERIFY(!QFileInfo::exists(local()));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.json"))));
    QCOMPARE(m_puller->localImagePath(), QString());
    QCOMPARE(m_conn->state(), Transport::State::Connected);

    // The connection is as good as before: the next pull completes (the slot above cancels once).
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QVERIFY2(readAll(local()) == big, "the pull after a cancel is not intact");
    QCOMPARE(started.count(), 2);
}

void Tst_imagepuller::cancelBeforeTransfer()
{
    RIG();
    const QByteArray image = rkfwImage(100 * 1024, 'a');
    QVERIFY(putRemote(kRemoteImage, image, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);

    m_puller->cancel();   // nothing running: ignored
    QCOMPARE(s.finished.count(), 0);

    // Cancelled while the stat is on its way: finished at once, the late answer is ignored.
    QVERIFY(m_puller->start());
    QVERIFY(m_puller->isRunning());
    m_puller->cancel();
    QCOMPARE(s.finished.count(), 1);
    QVERIFY(!s.ok(0));
    QCOMPARE(s.message(0), QStringLiteral("cancelled"));
    QVERIFY(!m_puller->isRunning());
    QTest::qWait(300);
    QCOMPARE(s.finished.count(), 1);
    QCOMPARE(s.info.count(), 0);
    QCOMPARE(started.count(), 0);
    QVERIFY(!QFileInfo::exists(local()));

    // And the puller is reusable.
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QCOMPARE(started.count(), 1);
    QVERIFY2(readAll(local()) == image, "pull after a cancel differs");
}

void Tst_imagepuller::destroyWhileRunning()
{
    RIG();
    QVERIFY(putRemote(kRemoteImage, rkfwImage(kBigImage, 'x'), kStampA));
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    QSignalSpy finished(m_conn.get(), &SshConnection::transferFinished);
    QSignalSpy pullerFinished(m_puller.get(), &ImagePuller::finished);
    QVERIFY(m_puller->start());
    QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, kTimeoutMs);
    m_puller.reset();   // the owner goes away in the middle of a download
    QCOMPARE(pullerFinished.count(), 0);   // the destroyed object reports nothing
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kTimeoutMs);   // the transfer was cancelled
    QVERIFY(!finished.at(0).at(0).toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!m_conn->isTransferActive(), kTimeoutMs);
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QVERIFY(!QFileInfo::exists(local()));
    QCOMPARE(m_conn->state(), Transport::State::Connected);
}

void Tst_imagepuller::connectionDropped()
{
    RIG();
    QVERIFY(putRemote(kRemoteImage, rkfwImage(kBigImage, 'x'), kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);

    bool dropped = false;
    QObject::connect(m_puller.get(), &ImagePuller::progress, m_puller.get(), [&] {
        if (!dropped) {
            dropped = true;
            m_server->dropAllClients();   // a network cut in the middle of the download
        }
    });
    QVERIFY(m_puller->start());
    QTRY_COMPARE_WITH_TIMEOUT(s.finished.count(), 1, kTimeoutMs);
    QVERIFY(dropped);
    QCOMPARE(started.count(), 1);
    QVERIFY(!s.ok(0));
    QVERIFY(!s.message(0).isEmpty());
    QVERIFY(!m_puller->isRunning());
    QTRY_COMPARE_WITH_TIMEOUT(m_conn->state(), Transport::State::Disconnected, kTimeoutMs);
    QVERIFY(!QFileInfo::exists(local()));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.new"))));
    QVERIFY(!QFileInfo::exists(local(QStringLiteral("update.img.json"))));
    QCOMPARE(m_puller->localImagePath(), QString());
    QTest::qWait(200);
    QCOMPARE(s.finished.count(), 1);   // once

    // A pull on the dead connection says so - after start() returned.
    QVERIFY(m_puller->start());
    QVERIFY(m_puller->isRunning());
    QCOMPARE(s.finished.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(s.finished.count(), 2, kTimeoutMs);
    QVERIFY(!s.ok(1));
    QCOMPARE(s.message(1), QStringLiteral("Not connected"));
    QVERIFY(!m_puller->isRunning());
}

void Tst_imagepuller::connectionClosedDuringStat()
{
    RIG();
    QVERIFY(putRemote(kRemoteImage, rkfwImage(100 * 1024, 'a'), kStampA));
    Spies s(m_puller.get());
    QVERIFY(m_puller->start());
    m_conn->close();   // the owner closes the session while the stat is on its way
    QCOMPARE(s.finished.count(), 1);
    QVERIFY(!s.ok(0));
    QVERIFY(!s.message(0).isEmpty());
    QVERIFY(!m_puller->isRunning());
    QTest::qWait(300);
    QCOMPARE(s.finished.count(), 1);
    QCOMPARE(s.info.count(), 0);
    QVERIFY(!QFileInfo::exists(local()));

    // Reconnect and pull: the old session's leftovers do not disturb the new run.
    QVERIFY(m_conn->open());
    QTRY_COMPARE_WITH_TIMEOUT(m_conn->state(), Transport::State::Connected, kTimeoutMs);
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QVERIFY(QFileInfo::exists(local()));
}

void Tst_imagepuller::startPrerequisitesAndBusy()
{
    // No connection: start() returns, then finished(false) arrives.
    {
        ImagePuller bare;
        bare.setRemoteFolder(kRemoteFolder);
        bare.setCacheDirectory(m_cache);
        QCOMPARE(bare.connection(), static_cast<SshConnection*>(nullptr));
        Spies s(&bare);
        QVERIFY(bare.start());
        QVERIFY(bare.isRunning());
        QCOMPARE(s.finished.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(s.finished.count(), 1, kTimeoutMs);
        QVERIFY(!s.ok(0));
        QCOMPARE(s.message(0), QStringLiteral("Not connected"));
        QVERIFY(!bare.isRunning());
        QVERIFY(!QFileInfo::exists(m_cache));
    }

    RIG();
    QVERIFY(putRemote(kRemoteImage, rkfwImage(100 * 1024, 'a'), kStampA));
    Spies s(m_puller.get());
    QCOMPARE(m_puller->remoteFolder(), kRemoteFolder);
    QCOMPARE(m_puller->cacheDirectory(), m_cache);
    QCOMPARE(m_puller->connection(), m_conn.get());

    m_puller->setRemoteFolder(QStringLiteral("   "));
    PULL(s);
    QVERIFY(!s.ok(0));
    QCOMPARE(s.message(0), QStringLiteral("No remote folder given"));

    m_puller->setRemoteFolder(kRemoteFolder);
    m_puller->setCacheDirectory(QString());
    PULL(s);
    QVERIFY(!s.ok(1));
    QCOMPARE(s.message(1), QStringLiteral("No cache folder given"));

    // A cache folder that cannot be created (a file is in the way).
    QVERIFY(writeFile(m_work + QStringLiteral("/afile"), QByteArrayLiteral("x")));
    m_puller->setCacheDirectory(m_work + QStringLiteral("/afile/cache"));
    PULL(s);
    QVERIFY(!s.ok(2));
    QVERIFY2(s.message(2).startsWith(QStringLiteral("Cannot create the cache folder")), qPrintable(s.message(2)));

    // One pull at a time; the connection cannot be swapped under it.
    m_puller->setCacheDirectory(m_cache);
    QVERIFY(m_puller->start());
    QVERIFY(!m_puller->start());
    SshConnection other;
    m_puller->setConnection(&other);
    QCOMPARE(m_puller->connection(), m_conn.get());
    QTRY_COMPARE_WITH_TIMEOUT(s.finished.count(), 4, kTimeoutMs);
    QVERIFY2(s.ok(3), qPrintable(s.message(3)));
    QVERIFY(!m_puller->isRunning());
    m_puller->setConnection(&other);   // idle again: allowed
    QCOMPARE(m_puller->connection(), &other);
    m_puller->setConnection(m_conn.get());
    PULL(s);
    QCOMPARE(s.skipped.count(), 1);
}

void Tst_imagepuller::remoteFolderForms()
{
    TestSshServer::Options options;
    options.gitDescribeOutput = kGit;
    RIG(options);
    const QByteArray image = rkfwImage(100 * 1024, 'a');
    QVERIFY(putRemote(kRemoteImage, image, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);
    const QString absoluteFolder = QDir::cleanPath(m_root + QStringLiteral("/rk/sdk/output/image"));

    // An absolute folder, with and without a trailing slash; the SDK is its grandparent.
    m_puller->setRemoteFolder(absoluteFolder);
    PULL(s);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));
    QCOMPARE(s.info.at(0).at(3).toString(), kGit);
    QVERIFY2(readAll(local()) == image, "absolute folder: image differs");
    QTRY_VERIFY_WITH_TIMEOUT(m_counters->execCommands.contains(
                                 QStringLiteral("git -C '%1' describe --tags --dirty 2>/dev/null")
                                     .arg(QDir::cleanPath(m_root + QStringLiteral("/rk/sdk")))),
                             kTimeoutMs);
    m_puller->setRemoteFolder(absoluteFolder + QLatin1Char('/'));
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QCOMPARE(s.skipped.count(), 1);   // the same image is recognised through the other spelling

    // "~/folder/" with a trailing slash.
    m_puller->setRemoteFolder(kRemoteFolder + QLatin1Char('/'));
    PULL(s);
    QVERIFY2(s.ok(2), qPrintable(s.message(2)));
    QCOMPARE(s.skipped.count(), 2);
    QCOMPARE(started.count(), 1);

    // "~" alone is the home folder: the image lives right there, the "SDK" is "." as well.
    const QByteArray homeImage = rkfwImage(80 * 1024, 'h');
    QVERIFY(putRemote(QStringLiteral("update.img"), homeImage, kStampB));
    m_puller->setRemoteFolder(QStringLiteral("~"));
    PULL(s);
    QVERIFY2(s.ok(3), qPrintable(s.message(3)));
    QVERIFY2(readAll(local()) == homeImage, "~ folder: image differs");
    QCOMPARE(s.info.at(3).at(3).toString(), kGit);
    QTRY_VERIFY_WITH_TIMEOUT(m_counters->execCommands.contains(QStringLiteral("git -C '.' describe --tags --dirty 2>/dev/null")),
                             kTimeoutMs);
    QVERIFY2(s.message(3).endsWith(QStringLiteral(" from %1:~").arg(m_conn->profile().displayTarget())), qPrintable(s.message(3)));
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteFolder, QStringLiteral("~"));
}

void Tst_imagepuller::shellFallbackPull()
{
    // A dropbear-like server: no SFTP subsystem, stat and the download go through exec channels.
    TestSshServer::Options options;
    options.allowSftp = false;
    options.gitDescribeOutput = kGit;
    RIG(options);
    const QByteArray v1 = rkfwImage(300 * 1024, 'a');
    const QByteArray v2 = rkfwImage(320 * 1024, 'b');
    QVERIFY(putRemote(kRemoteImage, v1, kStampA));
    Spies s(m_puller.get());
    QSignalSpy started(m_conn.get(), &SshConnection::transferStarted);

    PULL(s);
    QVERIFY2(s.ok(0), qPrintable(s.message(0)));
    QVERIFY2(readAll(local()) == v1, "image differs");
    QCOMPARE(s.info.at(0).at(1).toLongLong(), qint64(v1.size()));
    QCOMPARE(s.info.at(0).at(2).toDateTime().toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(s.info.at(0).at(3).toString(), kGit);
    QCOMPARE(started.count(), 1);
    QCOMPARE(ImagePuller::cachedImage(m_cache).remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    for (const QString& command : {QStringLiteral("stat -c '%s %Y %F' 'rk/sdk/output/image/update.img'"),
                                   QStringLiteral("git -C 'rk/sdk' describe --tags --dirty 2>/dev/null"),
                                   QStringLiteral("wc -c < 'rk/sdk/output/image/update.img'"),
                                   QStringLiteral("cat 'rk/sdk/output/image/update.img'")}) {
        QTRY_VERIFY2_WITH_TIMEOUT(m_counters->execCommands.contains(command), qPrintable(command), kTimeoutMs);
    }
    QCOMPARE(m_counters->sftp, 1);   // asked once, refused

    // Unchanged: nothing is downloaded.
    PULL(s);
    QVERIFY2(s.ok(1), qPrintable(s.message(1)));
    QCOMPARE(s.skipped.count(), 1);
    QCOMPARE(started.count(), 1);
    QCOMPARE(m_counters->execCommands.filter(QStringLiteral("cat 'rk/sdk/output/image/update.img'")).size(), 1);

    // A new build rotates the history over the shell as well.
    QVERIFY(putRemote(kRemoteImage, v2, kStampB));
    PULL(s);
    QVERIFY2(s.ok(2), qPrintable(s.message(2)));
    QVERIFY2(readAll(local()) == v2, "new build differs");
    QVERIFY2(readAll(ImagePuller::previousImagePath(m_cache)) == v1, "previous build differs");

    // Missing file: the shell's "No such file" is the plain "does not exist".
    QVERIFY(QFile::remove(QDir(m_root).filePath(kRemoteImage)));
    PULL(s);
    QVERIFY(!s.ok(3));
    QVERIFY2(s.message(3).startsWith(QStringLiteral("no update.img in ")), qPrintable(s.message(3)));
    QVERIFY2(readAll(local()) == v2, "a missing remote file touched the cache");
}

void Tst_imagepuller::cacheNotes()
{
    const QString cache = m_work + QStringLiteral("/notes");
    QVERIFY(QDir().mkpath(cache));
    QCOMPARE(ImagePuller::imagePath(cache), QDir(cache).filePath(QStringLiteral("update.img")));
    QCOMPARE(ImagePuller::previousImagePath(cache), QDir(cache).filePath(QStringLiteral("update.prev.img")));
    QCOMPARE(QString::fromLatin1(ImagePuller::kImageName), QStringLiteral("update.img"));
    QCOMPARE(QString::fromLatin1(ImagePuller::kPreviousImageName), QStringLiteral("update.prev.img"));
    QCOMPARE(QString::fromLatin1(ImagePuller::kNoteSuffix), QStringLiteral(".json"));

    // Nothing there: invalid, empty summary.
    QVERIFY(!ImagePuller::cachedImage(cache).isValid());
    QCOMPARE(ImagePuller::cachedImage(cache).summary(), QString());
    QVERIFY(!ImagePuller::readNote(QDir(cache).filePath(QStringLiteral("other.img"))).isValid());

    // An image without a note: the file facts only.
    const QString image = ImagePuller::imagePath(cache);
    QVERIFY(writeFile(image, rkfwImage(2000, 'a')));
    ImagePuller::CacheEntry bare = ImagePuller::cachedImage(cache);
    QVERIFY(bare.isValid());
    QCOMPARE(bare.path, image);
    QCOMPARE(bare.size, qint64(2000));
    QVERIFY(!bare.remoteModified.isValid());
    QVERIFY(!bare.pulledAt.isValid());
    QVERIFY(bare.target.isEmpty() && bare.remoteFolder.isEmpty() && bare.gitDescribe.isEmpty());
    QCOMPARE(bare.summary(), QStringLiteral("update.img 2.0 kB"));

    // Write and read back.
    ImagePuller::CacheEntry entry;
    entry.path = image;
    entry.size = 2000;
    entry.remoteModified = kStampA;
    entry.pulledAt = kStampB;
    entry.target = QStringLiteral("moses@10.0.10.71");
    entry.remoteFolder = QStringLiteral("~/rk/rockchip-platforms/output/image");
    entry.gitDescribe = kGit;
    QString error;
    QVERIFY2(ImagePuller::writeNote(entry, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(image + QStringLiteral(".json")));
    const ImagePuller::CacheEntry back = ImagePuller::readNote(image);
    QVERIFY(back.isValid());
    QCOMPARE(back.path, image);
    QCOMPARE(back.size, qint64(2000));
    QCOMPARE(back.remoteModified.toSecsSinceEpoch(), kStampA.toSecsSinceEpoch());
    QCOMPARE(back.pulledAt.toSecsSinceEpoch(), kStampB.toSecsSinceEpoch());
    QCOMPARE(back.target, entry.target);
    QCOMPARE(back.remoteFolder, entry.remoteFolder);
    QCOMPARE(back.gitDescribe, kGit);
    QCOMPARE(ImagePuller::cachedImage(cache).gitDescribe, kGit);
    QVERIFY(!ImagePuller::previousImage(cache).isValid());

    // The note is plain JSON a human can read.
    const QByteArray json = readAll(image + QStringLiteral(".json"));
    QVERIFY2(json.contains("\"target\"") && json.contains("moses@10.0.10.71") && json.contains(kGit.toUtf8()), json.constData());

    // A note for another file (the size differs): ignored, the file facts stay.
    QVERIFY(writeFile(image, rkfwImage(2500, 'b')));
    const ImagePuller::CacheEntry stale = ImagePuller::readNote(image);
    QVERIFY(stale.isValid());
    QCOMPARE(stale.size, qint64(2500));
    QVERIFY(!stale.remoteModified.isValid());
    QVERIFY(stale.gitDescribe.isEmpty());

    // A damaged note: the same.
    QVERIFY(writeFile(image + QStringLiteral(".json"), QByteArrayLiteral("{ not json")));
    const ImagePuller::CacheEntry damaged = ImagePuller::readNote(image);
    QVERIFY(damaged.isValid());
    QVERIFY(!damaged.remoteModified.isValid());
    QVERIFY(writeFile(image + QStringLiteral(".json"), QByteArrayLiteral("[1, 2]")));
    QVERIFY(ImagePuller::readNote(image).isValid());

    // A note without its image is nothing.
    QVERIFY(QFile::remove(image));
    QVERIFY(!ImagePuller::readNote(image).isValid());

    // writeNote needs a path and a writable place.
    ImagePuller::CacheEntry nowhere;
    error.clear();
    QVERIFY(!ImagePuller::writeNote(nowhere, &error));
    QVERIFY(!error.isEmpty());
    nowhere.path = m_work + QStringLiteral("/no/such/folder/update.img");
    error.clear();
    QVERIFY(!ImagePuller::writeNote(nowhere, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!ImagePuller::writeNote(nowhere));   // the error text is optional

    // The summary the tab shows (the example of ImagePuller.h).
    ImagePuller::CacheEntry shown;
    shown.path = QStringLiteral("cache/update.img");
    shown.size = 11354755;
    shown.remoteModified = QDateTime(QDate(2026, 10, 13), QTime(21, 45), QTimeZone::LocalTime);
    shown.pulledAt = QDateTime(QDate(2026, 10, 13), QTime(10, 40), QTimeZone::LocalTime);
    shown.target = QStringLiteral("moses@10.0.10.71");
    shown.gitDescribe = QStringLiteral("v1.3.0-4-ge0e0d41");
    QCOMPARE(shown.summary(),
             QStringLiteral("update.img 11.4 MB, built 2026-10-13 21:45 (v1.3.0-4-ge0e0d41), pulled 2026-10-13 10:40 from moses@10.0.10.71"));
    shown.gitDescribe.clear();
    shown.pulledAt = QDateTime();
    shown.target.clear();
    QCOMPARE(shown.summary(), QStringLiteral("update.img 11.4 MB, built 2026-10-13 21:45"));
}

QTEST_GUILESS_MAIN(Tst_imagepuller)
#include "tst_imagepuller.moc"
