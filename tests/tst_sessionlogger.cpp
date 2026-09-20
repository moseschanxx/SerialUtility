#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>

#include "core/SessionLogger.h"

namespace {

const QString kHeader = QStringLiteral("COM8 115200 8N1");
const QByteArray kHeaderPrefix = QByteArrayLiteral("# BuildAI Serial Utility log - COM8 115200 8N1 - started ");

} // namespace

/// UI-free checks of SessionLogger's file handling (header placement, append semantics,
/// start()/signal contract). Traffic formatting is covered by tst_sessionwidget.
class Tst_sessionlogger : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void headerOnFreshLineAfterUnterminatedTail();
    void headerWithoutBlankLineAfterTerminatedTail();
    void headerAtStartOfEmptyOrMissingFile();
    void rawFormatWritesNothingOnStart();
    void startResultMatchesActiveStateAndSignals();
    void startFailureEmitsErrorOnly();

private:
    QString writeFile(const QString& name, const QByteArray& content);
    static QByteArray readFile(const QString& path);
    QTemporaryDir m_dir;
};

void Tst_sessionlogger::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

QString Tst_sessionlogger::writeFile(const QString& name, const QByteArray& content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    file.write(content);
    file.close();
    return path;
}

QByteArray Tst_sessionlogger::readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

void Tst_sessionlogger::headerOnFreshLineAfterUnterminatedTail()
{
    // A previous capture usually ends in a prompt without '\n'; the header must not be glued to it.
    const QString path = writeFile(QStringLiteral("unterminated.log"), QByteArrayLiteral("abc"));
    QVERIFY(!path.isEmpty());

    SessionLogger logger;
    QVERIFY(logger.start(path, SessionLogger::Format::Text, true, kHeader));
    logger.stop();

    const QByteArray content = readFile(path);
    QVERIFY2(content.startsWith(QByteArrayLiteral("abc\n") + kHeaderPrefix), content.constData());
    QVERIFY(content.endsWith('\n'));
    QCOMPARE(content.count(QByteArrayLiteral("BuildAI")), qsizetype(1));
}

void Tst_sessionlogger::headerWithoutBlankLineAfterTerminatedTail()
{
    const QString path = writeFile(QStringLiteral("terminated.log"), QByteArrayLiteral("abc\n"));
    QVERIFY(!path.isEmpty());

    SessionLogger logger;
    QVERIFY(logger.start(path, SessionLogger::Format::Text, true, kHeader));
    logger.stop();

    const QByteArray content = readFile(path);
    QVERIFY2(content.startsWith(QByteArrayLiteral("abc\n") + kHeaderPrefix), content.constData());
    QVERIFY2(!content.contains(QByteArrayLiteral("\n\n")), content.constData());
}

void Tst_sessionlogger::headerAtStartOfEmptyOrMissingFile()
{
    // Empty existing file.
    const QString emptyPath = writeFile(QStringLiteral("empty.log"), QByteArray());
    QVERIFY(!emptyPath.isEmpty());
    {
        SessionLogger logger;
        QVERIFY(logger.start(emptyPath, SessionLogger::Format::Text, true, kHeader));
        logger.stop();
    }
    const QByteArray emptyContent = readFile(emptyPath);
    QVERIFY2(emptyContent.startsWith(kHeaderPrefix), emptyContent.constData());

    // Nonexistent file in a directory that has to be created.
    const QString missingPath = m_dir.filePath(QStringLiteral("sub/dir/missing.log"));
    QVERIFY(!QFile::exists(missingPath));
    {
        SessionLogger logger;
        QVERIFY(logger.start(missingPath, SessionLogger::Format::HexDump, true, kHeader));
        logger.stop();
    }
    const QByteArray missingContent = readFile(missingPath);
    QVERIFY2(missingContent.startsWith(kHeaderPrefix), missingContent.constData());
}

void Tst_sessionlogger::rawFormatWritesNothingOnStart()
{
    // Raw captures are a verbatim byte stream: no header and no inserted newline, ever.
    const QString path = writeFile(QStringLiteral("raw.bin"), QByteArrayLiteral("root@rv1106:~# "));
    QVERIFY(!path.isEmpty());
    const QByteArray before = readFile(path);

    SessionLogger logger;
    QVERIFY(logger.start(path, SessionLogger::Format::Raw, true, kHeader));
    QCOMPARE(logger.bytesWritten(), qint64(0));
    logger.stop();

    QCOMPARE(readFile(path), before);
}

void Tst_sessionlogger::startResultMatchesActiveStateAndSignals()
{
    const QString path = m_dir.filePath(QStringLiteral("signals.log"));
    SessionLogger logger;
    QSignalSpy startedSpy(&logger, &SessionLogger::started);
    QSignalSpy stoppedSpy(&logger, &SessionLogger::stopped);
    QSignalSpy errorSpy(&logger, &SessionLogger::error);

    const bool ok = logger.start(path, SessionLogger::Format::Text, true, kHeader);
    QCOMPARE(ok, logger.isActive());
    QVERIFY(ok);
    QCOMPARE(logger.filePath(), path);
    QCOMPARE(logger.format(), SessionLogger::Format::Text);
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).toString(), path);
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(logger.bytesWritten() > 0);   // header reached the OS

    logger.logReceived(QByteArrayLiteral("hello\n"));
    const qint64 bytes = logger.bytesWritten();
    QVERIFY(bytes > 0);

    logger.stop();
    QVERIFY(!logger.isActive());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).toString(), path);
    QCOMPARE(stoppedSpy.at(0).at(1).toLongLong(), bytes);
    QCOMPARE(readFile(path).size(), qsizetype(bytes));
    logger.stop();   // idempotent
    QCOMPARE(stoppedSpy.count(), 1);
}

void Tst_sessionlogger::startFailureEmitsErrorOnly()
{
    // A directory path opens for neither writing nor appending on any platform.
    const QString dirPath = m_dir.filePath(QStringLiteral("iamadir"));
    QVERIFY(QDir().mkpath(dirPath));

    SessionLogger logger;
    QSignalSpy startedSpy(&logger, &SessionLogger::started);
    QSignalSpy errorSpy(&logger, &SessionLogger::error);

    QVERIFY(!logger.start(dirPath, SessionLogger::Format::Text, true, kHeader));
    QVERIFY(!logger.isActive());
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 1);

    // Empty name: same contract.
    QVERIFY(!logger.start(QStringLiteral("   "), SessionLogger::Format::Text, true, kHeader));
    QVERIFY(!logger.isActive());
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 2);

    // Whatever start() returns, started() has fired exactly that many times in total.
    const QString path = m_dir.filePath(QStringLiteral("after-failure.log"));
    const bool ok = logger.start(path, SessionLogger::Format::Text, true, kHeader);
    QCOMPARE(startedSpy.count(), ok ? 1 : 0);
    QCOMPARE(ok, logger.isActive());
}

QTEST_GUILESS_MAIN(Tst_sessionlogger)
#include "tst_sessionlogger.moc"
