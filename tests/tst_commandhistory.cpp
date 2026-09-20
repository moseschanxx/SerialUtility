#include <QtTest>
#include <QTemporaryDir>

#include "core/CommandHistory.h"

class Tst_commandhistory : public QObject
{
    Q_OBJECT
private slots:
    void emptyHistory();
    void addIgnoresBlank();
    void addDedupesNewestOnly();
    void addCapsAtMax();
    void navigationUpAndDown();
    void previousStaysAtOldest();
    void nextReturnsBlankBelowNewest();
    void addResetsNavigation();
    void resetNavigation();
    void setEntriesTrimsAndResets();
    void setMaxEntriesDropsOldest();
    void clearResets();
    void saveAndLoad();
    void loadMissingFile();
    void saveCreatesDirectory();
    void copyIsIndependent();
};

void Tst_commandhistory::emptyHistory()
{
    CommandHistory h;
    QCOMPARE(h.size(), 0);
    QCOMPARE(h.maxEntries(), 500);
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.previous(), QString());
    QCOMPARE(h.next(), QString());
    QVERIFY(!h.isNavigating());
    QVERIFY(h.entries().isEmpty());
}

void Tst_commandhistory::addIgnoresBlank()
{
    CommandHistory h;
    h.add(QString());
    h.add(QStringLiteral("   "));
    h.add(QStringLiteral("\t\n"));
    QCOMPARE(h.size(), 0);
    h.add(QStringLiteral("ls"));
    QCOMPARE(h.size(), 1);
    // Leading/trailing whitespace is kept for non-blank commands.
    h.add(QStringLiteral(" ls "));
    QCOMPARE(h.size(), 2);
    QCOMPARE(h.entries().last(), QStringLiteral(" ls "));
}

void Tst_commandhistory::addDedupesNewestOnly()
{
    CommandHistory h;
    h.add(QStringLiteral("a"));
    h.add(QStringLiteral("a"));
    QCOMPARE(h.size(), 1);
    h.add(QStringLiteral("b"));
    h.add(QStringLiteral("a"));   // not the newest -> appended again
    QCOMPARE(h.size(), 3);
    QCOMPARE(h.entries(), QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a")}));
    h.add(QStringLiteral("a"));
    QCOMPARE(h.size(), 3);
}

void Tst_commandhistory::addCapsAtMax()
{
    CommandHistory h(3);
    QCOMPARE(h.maxEntries(), 3);
    for (int i = 1; i <= 5; ++i) {
        h.add(QStringLiteral("cmd%1").arg(i));
    }
    QCOMPARE(h.size(), 3);
    QCOMPARE(h.entries(), QStringList({QStringLiteral("cmd3"), QStringLiteral("cmd4"), QStringLiteral("cmd5")}));

    // A max below 1 is clamped to 1.
    CommandHistory tiny(0);
    QCOMPARE(tiny.maxEntries(), 1);
    tiny.add(QStringLiteral("x"));
    tiny.add(QStringLiteral("y"));
    QCOMPARE(tiny.entries(), QStringList({QStringLiteral("y")}));
}

void Tst_commandhistory::navigationUpAndDown()
{
    CommandHistory h;
    h.add(QStringLiteral("a"));
    h.add(QStringLiteral("b"));
    h.add(QStringLiteral("c"));
    QVERIFY(!h.isNavigating());

    QCOMPARE(h.previous(), QStringLiteral("c"));
    QVERIFY(h.isNavigating());
    QCOMPARE(h.previous(), QStringLiteral("b"));
    QCOMPARE(h.previous(), QStringLiteral("a"));
    QCOMPARE(h.next(), QStringLiteral("b"));
    QCOMPARE(h.next(), QStringLiteral("c"));
    QVERIFY(h.isNavigating());
    QCOMPARE(h.next(), QString());   // back to the blank line
    QVERIFY(!h.isNavigating());
}

void Tst_commandhistory::previousStaysAtOldest()
{
    CommandHistory h;
    h.add(QStringLiteral("first"));
    h.add(QStringLiteral("second"));
    QCOMPARE(h.previous(), QStringLiteral("second"));
    QCOMPARE(h.previous(), QStringLiteral("first"));
    QCOMPARE(h.previous(), QStringLiteral("first"));
    QCOMPARE(h.previous(), QStringLiteral("first"));
    QVERIFY(h.isNavigating());
    QCOMPARE(h.next(), QStringLiteral("second"));
}

void Tst_commandhistory::nextReturnsBlankBelowNewest()
{
    CommandHistory h;
    h.add(QStringLiteral("only"));
    // next() from the blank line stays on the blank line (the draft the user typed).
    QCOMPARE(h.next(), QString());
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.next(), QString());
    QCOMPARE(h.previous(), QStringLiteral("only"));
    QCOMPARE(h.next(), QString());
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.next(), QString());
    // And going up again starts from the newest entry.
    QCOMPARE(h.previous(), QStringLiteral("only"));
}

void Tst_commandhistory::addResetsNavigation()
{
    CommandHistory h;
    h.add(QStringLiteral("a"));
    h.add(QStringLiteral("b"));
    QCOMPARE(h.previous(), QStringLiteral("b"));
    QCOMPARE(h.previous(), QStringLiteral("a"));
    h.add(QStringLiteral("c"));
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.previous(), QStringLiteral("c"));
    // Even an ignored (blank) add resets the cursor.
    h.add(QStringLiteral("  "));
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.previous(), QStringLiteral("c"));
    // Adding a duplicate of the newest entry resets too.
    h.previous();
    h.add(QStringLiteral("c"));
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.size(), 3);
}

void Tst_commandhistory::resetNavigation()
{
    CommandHistory h;
    h.add(QStringLiteral("a"));
    h.add(QStringLiteral("b"));
    h.previous();
    h.previous();
    QVERIFY(h.isNavigating());
    h.resetNavigation();
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.next(), QString());
    QCOMPARE(h.previous(), QStringLiteral("b"));
}

void Tst_commandhistory::setEntriesTrimsAndResets()
{
    CommandHistory h(2);
    h.add(QStringLiteral("old"));
    h.previous();
    h.setEntries({QStringLiteral("one"), QStringLiteral(""), QStringLiteral("two"), QStringLiteral("three")});
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.entries(), QStringList({QStringLiteral("two"), QStringLiteral("three")}));
    QCOMPARE(h.previous(), QStringLiteral("three"));
}

void Tst_commandhistory::setMaxEntriesDropsOldest()
{
    CommandHistory h(10);
    for (int i = 0; i < 5; ++i) {
        h.add(QString::number(i));
    }
    h.setMaxEntries(2);
    QCOMPARE(h.maxEntries(), 2);
    QCOMPARE(h.entries(), QStringList({QStringLiteral("3"), QStringLiteral("4")}));
    QVERIFY(!h.isNavigating());
    // Growing keeps everything.
    h.setMaxEntries(100);
    QCOMPARE(h.size(), 2);
    h.add(QStringLiteral("5"));
    QCOMPARE(h.size(), 3);
}

void Tst_commandhistory::clearResets()
{
    CommandHistory h;
    h.add(QStringLiteral("a"));
    h.previous();
    h.clear();
    QCOMPARE(h.size(), 0);
    QVERIFY(!h.isNavigating());
    QCOMPARE(h.previous(), QString());
    QCOMPARE(h.maxEntries(), 500);
}

void Tst_commandhistory::saveAndLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("history.txt"));

    CommandHistory h;
    h.add(QStringLiteral("uname -a"));
    h.add(QString::fromUtf8("echo 中文"));
    h.add(QStringLiteral("cat /proc/cpuinfo"));
    QVERIFY(h.save(path));
    QVERIFY(QFile::exists(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();
    QVERIFY(raw.contains(QString::fromUtf8("echo 中文").toUtf8()));

    CommandHistory loaded(3);
    QVERIFY(loaded.load(path));
    QCOMPARE(loaded.entries(), h.entries());
    QVERIFY(!loaded.isNavigating());
    QCOMPARE(loaded.previous(), QStringLiteral("cat /proc/cpuinfo"));

    // Loading into a smaller history keeps the newest entries.
    CommandHistory small(2);
    QVERIFY(small.load(path));
    QCOMPARE(small.entries(), QStringList({QString::fromUtf8("echo 中文"), QStringLiteral("cat /proc/cpuinfo")}));

    // Saving an empty history produces an empty file that loads back empty.
    CommandHistory empty;
    QVERIFY(empty.save(path));
    QVERIFY(loaded.load(path));
    QCOMPARE(loaded.size(), 0);
}

void Tst_commandhistory::loadMissingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CommandHistory h;
    h.add(QStringLiteral("keep"));
    QVERIFY(!h.load(dir.filePath(QStringLiteral("does_not_exist.txt"))));
    // Contents are untouched on failure.
    QCOMPARE(h.entries(), QStringList({QStringLiteral("keep")}));
}

void Tst_commandhistory::saveCreatesDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nested/deeper/history.txt"));
    CommandHistory h;
    h.add(QStringLiteral("x"));
    QVERIFY(h.save(path));
    QVERIFY(QFile::exists(path));

    // Saving to a path whose parent is a regular file must fail cleanly.
    const QString blocker = dir.filePath(QStringLiteral("file.txt"));
    QVERIFY(h.save(blocker));
    QVERIFY(!h.save(blocker + QStringLiteral("/history.txt")));
}

void Tst_commandhistory::copyIsIndependent()
{
    CommandHistory a;
    a.add(QStringLiteral("one"));
    CommandHistory b = a;
    b.add(QStringLiteral("two"));
    QCOMPARE(a.size(), 1);
    QCOMPARE(b.size(), 2);
    QCOMPARE(b.previous(), QStringLiteral("two"));
    QVERIFY(!a.isNavigating());
}

QTEST_GUILESS_MAIN(Tst_commandhistory)
#include "tst_commandhistory.moc"
