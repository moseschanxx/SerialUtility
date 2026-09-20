#include <QtTest>

#include "core/LineEnding.h"

// LineEnding::toString(Mode) is found by ADL from QTest::toString and returns a QString, so
// enum values are compared through their integer value.
#define QCOMPARE_MODE(actual, expected) QCOMPARE(static_cast<int>(actual), static_cast<int>(expected))

using LineEnding::Mode;

class Tst_lineending : public QObject
{
    Q_OBJECT
private slots:
    void bytesPerMode();
    void displayNames();
    void allModesOrder();
    void toStringKeys();
    void fromStringKeys();
    void roundTrip();
};

void Tst_lineending::bytesPerMode()
{
    QCOMPARE(LineEnding::bytes(Mode::None), QByteArray());
    QCOMPARE(LineEnding::bytes(Mode::CR), QByteArray("\r"));
    QCOMPARE(LineEnding::bytes(Mode::LF), QByteArray("\n"));
    QCOMPARE(LineEnding::bytes(Mode::CRLF), QByteArray("\r\n"));
}

void Tst_lineending::displayNames()
{
    QCOMPARE(LineEnding::displayName(Mode::None), QStringLiteral("None"));
    QCOMPARE(LineEnding::displayName(Mode::CR), QStringLiteral("CR (\\r)"));
    QCOMPARE(LineEnding::displayName(Mode::LF), QStringLiteral("LF (\\n)"));
    QVERIFY(LineEnding::displayName(Mode::CRLF).contains(QStringLiteral("\\r\\n")));
    // Every name is unique so combo boxes stay unambiguous.
    QSet<QString> names;
    for (Mode mode : LineEnding::allModes()) {
        names.insert(LineEnding::displayName(mode));
    }
    QCOMPARE(names.size(), 4);
}

void Tst_lineending::allModesOrder()
{
    const QList<Mode> modes = LineEnding::allModes();
    QCOMPARE(modes.size(), 4);
    QCOMPARE_MODE(modes.at(0), Mode::None);
    QCOMPARE_MODE(modes.at(1), Mode::CR);
    QCOMPARE_MODE(modes.at(2), Mode::LF);
    QCOMPARE_MODE(modes.at(3), Mode::CRLF);
}

void Tst_lineending::toStringKeys()
{
    QCOMPARE(LineEnding::toString(Mode::None), QStringLiteral("none"));
    QCOMPARE(LineEnding::toString(Mode::CR), QStringLiteral("cr"));
    QCOMPARE(LineEnding::toString(Mode::LF), QStringLiteral("lf"));
    QCOMPARE(LineEnding::toString(Mode::CRLF), QStringLiteral("crlf"));
}

void Tst_lineending::fromStringKeys()
{
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("none")), Mode::None);
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("cr")), Mode::CR);
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("lf")), Mode::LF);
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("crlf")), Mode::CRLF);
    // Case and surrounding whitespace are tolerated.
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral(" CRLF ")), Mode::CRLF);
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("Lf")), Mode::LF);
    // Unknown -> CR.
    QCOMPARE_MODE(LineEnding::fromString(QString()), Mode::CR);
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("bogus")), Mode::CR);
    QCOMPARE_MODE(LineEnding::fromString(QStringLiteral("\\r\\n")), Mode::CR);
}

void Tst_lineending::roundTrip()
{
    for (Mode mode : LineEnding::allModes()) {
        QCOMPARE_MODE(LineEnding::fromString(LineEnding::toString(mode)), mode);
    }
}

QTEST_GUILESS_MAIN(Tst_lineending)
#include "tst_lineending.moc"
