#include <QtTest>

#include "core/HexUtils.h"

class Tst_hexutils : public QObject
{
    Q_OBJECT
private slots:
    // parseHexString
    void parsePlainPairs();
    void parseSeparators();
    void parsePrefixes();
    void parseEmpty();
    void parseOddNibbles();
    void parseInvalidCharacter();
    void parseClearsOutputOnError();

    // toHexString
    void toHexUpperLower();
    void toHexSeparator();
    void toHexEmpty();

    // hexDump
    void hexDumpHeaderExample();
    void hexDumpTwentyBytes();
    void hexDumpExactlySixteenBytes();
    void hexDumpEmpty();
    void hexDumpBaseOffset();
    void hexDumpEightPerLine();
    void hexDumpNonPrintable();

    // printableAscii
    void printable();

    // unescape
    void unescapeSimpleEscapes();
    void unescapeHex();
    void unescapeUnicode();
    void unescapeSurrogatePair();
    void unescapePlainUtf8();
    void unescapeErrors();
    void unescapeClearsError();
    void unescapeWithEncoder();

    // escapeForDisplay
    void escapeControls();
    void escapeUtf8();
    void escapeInvalidUtf8();
    void escapeRoundTrip();
};

// ---------------------------------------------------------------------------------------

void Tst_hexutils::parsePlainPairs()
{
    QByteArray out;
    QString error;
    QVERIFY(HexUtils::parseHexString(QStringLiteral("aabb"), out, &error));
    QCOMPARE(out, QByteArray::fromHex("aabb"));
    QVERIFY(error.isEmpty());

    QVERIFY(HexUtils::parseHexString(QStringLiteral("AA BB cc"), out));
    QCOMPARE(out, QByteArray::fromHex("aabbcc"));

    QVERIFY(HexUtils::parseHexString(QStringLiteral("00FF7f"), out));
    QCOMPARE(out, QByteArray::fromHex("00ff7f"));
}

void Tst_hexutils::parseSeparators()
{
    QByteArray out;
    QVERIFY(HexUtils::parseHexString(QStringLiteral("01,02:03-04 05\t06"), out));
    QCOMPARE(out, QByteArray::fromHex("010203040506"));

    QVERIFY(HexUtils::parseHexString(QStringLiteral("  0d   0a  "), out));
    QCOMPARE(out, QByteArray::fromHex("0d0a"));

    // A separator may even split a pair: "a a" is still one byte.
    QVERIFY(HexUtils::parseHexString(QStringLiteral("a a"), out));
    QCOMPARE(out, QByteArray::fromHex("aa"));
}

void Tst_hexutils::parsePrefixes()
{
    QByteArray out;
    QVERIFY(HexUtils::parseHexString(QStringLiteral("0xAA,0x0d"), out));
    QCOMPARE(out, QByteArray::fromHex("aa0d"));

    QVERIFY(HexUtils::parseHexString(QStringLiteral("0XAA 0Xbb"), out));
    QCOMPARE(out, QByteArray::fromHex("aabb"));

    // "0x" is only a prefix at a pair boundary; "000x" is 0x00 followed by a prefix.
    QVERIFY(HexUtils::parseHexString(QStringLiteral("000x11"), out));
    QCOMPARE(out, QByteArray::fromHex("0011"));
}

void Tst_hexutils::parseEmpty()
{
    QByteArray out("junk");
    QString error = QStringLiteral("stale");
    QVERIFY(HexUtils::parseHexString(QString(), out, &error));
    QVERIFY(out.isEmpty());
    QVERIFY(error.isEmpty());

    QVERIFY(HexUtils::parseHexString(QStringLiteral("   \t "), out, &error));
    QVERIFY(out.isEmpty());
}

void Tst_hexutils::parseOddNibbles()
{
    QByteArray out;
    QString error;
    QVERIFY(!HexUtils::parseHexString(QStringLiteral("abc"), out, &error));
    QVERIFY(out.isEmpty());
    QVERIFY(error.contains(QStringLiteral("Odd"), Qt::CaseInsensitive));

    QVERIFY(!HexUtils::parseHexString(QStringLiteral("a"), out));
    QVERIFY(!HexUtils::parseHexString(QStringLiteral("aa bb c"), out));
    // A null error pointer must be accepted.
    QVERIFY(!HexUtils::parseHexString(QStringLiteral("abc"), out, nullptr));
}

void Tst_hexutils::parseInvalidCharacter()
{
    QByteArray out;
    QString error;
    QVERIFY(!HexUtils::parseHexString(QStringLiteral("aa gg"), out, &error));
    QVERIFY(out.isEmpty());
    QVERIFY(error.contains(QStringLiteral("'g'")));
    QVERIFY(error.contains(QStringLiteral("position 4")));

    QVERIFY(!HexUtils::parseHexString(QStringLiteral("0xZZ"), out, &error));
    QVERIFY(error.contains(QStringLiteral("'Z'")));
}

void Tst_hexutils::parseClearsOutputOnError()
{
    QByteArray out("previous");
    QVERIFY(!HexUtils::parseHexString(QStringLiteral("aa bb x"), out));
    QVERIFY(out.isEmpty());
}

// ---------------------------------------------------------------------------------------

void Tst_hexutils::toHexUpperLower()
{
    const QByteArray hello("Hello");
    QCOMPARE(HexUtils::toHexString(hello), QStringLiteral("48 65 6C 6C 6F"));
    QCOMPARE(HexUtils::toHexString(hello, QStringLiteral(" "), false), QStringLiteral("48 65 6c 6c 6f"));
    QCOMPARE(HexUtils::toHexString(QByteArray::fromHex("00ff0a")), QStringLiteral("00 FF 0A"));
}

void Tst_hexutils::toHexSeparator()
{
    const QByteArray data = QByteArray::fromHex("0102ab");
    QCOMPARE(HexUtils::toHexString(data, QString()), QStringLiteral("0102AB"));
    QCOMPARE(HexUtils::toHexString(data, QStringLiteral(", ")), QStringLiteral("01, 02, AB"));
    QCOMPARE(HexUtils::toHexString(QByteArray(1, '\x7f'), QStringLiteral(":")), QStringLiteral("7F"));
}

void Tst_hexutils::toHexEmpty()
{
    QVERIFY(HexUtils::toHexString(QByteArray()).isEmpty());
}

// ---------------------------------------------------------------------------------------

void Tst_hexutils::hexDumpHeaderExample()
{
    // The example from HexUtils.h, byte for byte.
    const QByteArray data("Hello World\r\n");
    const QString expected = QStringLiteral("00000000  48 65 6C 6C 6F 20 57 6F  72 6C 64 0D 0A") +
                             QString(11, QLatin1Char(' ')) + QStringLiteral("|Hello World..|");
    QCOMPARE(HexUtils::hexDump(data), expected);
    QCOMPARE(HexUtils::hexDump(data),
             QStringLiteral("00000000  48 65 6C 6C 6F 20 57 6F  72 6C 64 0D 0A           |Hello World..|"));
}

void Tst_hexutils::hexDumpTwentyBytes()
{
    const QByteArray data("0123456789ABCDEFGHIJ");
    QCOMPARE(data.size(), 20);
    const QString line1 =
        QStringLiteral("00000000  30 31 32 33 34 35 36 37  38 39 41 42 43 44 45 46  |0123456789ABCDEF|");
    const QString line2 =
        QStringLiteral("00000010  47 48 49 4A") + QString(39, QLatin1Char(' ')) + QStringLiteral("|GHIJ|");
    const QString dump = HexUtils::hexDump(data);
    QCOMPARE(dump, line1 + QLatin1Char('\n') + line2);
    QVERIFY(!dump.endsWith(QLatin1Char('\n')));

    // The '|' column is at the same index on both lines.
    const QStringList lines = dump.split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0).indexOf(QLatin1Char('|')), lines.at(1).indexOf(QLatin1Char('|')));
    QCOMPARE(lines.at(0).indexOf(QLatin1Char('|')), 60);
}

void Tst_hexutils::hexDumpExactlySixteenBytes()
{
    QByteArray data;
    for (int i = 0; i < 16; ++i) {
        data.append(static_cast<char>(0x41 + i));   // 'A'..'P'
    }
    const QString expected =
        QStringLiteral("00000000  41 42 43 44 45 46 47 48  49 4A 4B 4C 4D 4E 4F 50  |ABCDEFGHIJKLMNOP|");
    QCOMPARE(HexUtils::hexDump(data), expected);
    QCOMPARE(HexUtils::hexDump(data).count(QLatin1Char('\n')), 0);
}

void Tst_hexutils::hexDumpEmpty()
{
    QVERIFY(HexUtils::hexDump(QByteArray()).isEmpty());
    QVERIFY(HexUtils::hexDump(QByteArray(), 0x1000).isEmpty());
}

void Tst_hexutils::hexDumpBaseOffset()
{
    const QByteArray data(17, 'x');
    const QStringList lines = HexUtils::hexDump(data, 0x1F0).split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QVERIFY(lines.at(0).startsWith(QStringLiteral("000001F0  ")));
    QVERIFY(lines.at(1).startsWith(QStringLiteral("00000200  78")));
    QVERIFY(lines.at(1).endsWith(QStringLiteral("|x|")));
}

void Tst_hexutils::hexDumpEightPerLine()
{
    const QByteArray data("ABCDEFGHIJ");
    const QStringList lines = HexUtils::hexDump(data, 0, 8).split(QLatin1Char('\n'));
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QStringLiteral("00000000  41 42 43 44 45 46 47 48  |ABCDEFGH|"));
    // hex field is 8*3-1 = 23 wide: "49 4A" (5) + 18 fill + the 2 column spaces = 20 spaces
    QCOMPARE(lines.at(1), QStringLiteral("00000008  49 4A") + QString(20, QLatin1Char(' ')) + QStringLiteral("|IJ|"));
    QCOMPARE(lines.at(0).indexOf(QLatin1Char('|')), lines.at(1).indexOf(QLatin1Char('|')));
}

void Tst_hexutils::hexDumpNonPrintable()
{
    const QByteArray data = QByteArray::fromHex("001b7f80ff20");
    const QString dump = HexUtils::hexDump(data);
    QVERIFY(dump.startsWith(QStringLiteral("00000000  00 1B 7F 80 FF 20")));
    QVERIFY(dump.endsWith(QStringLiteral("|..... |")));
}

// ---------------------------------------------------------------------------------------

void Tst_hexutils::printable()
{
    QCOMPARE(HexUtils::printableAscii(QByteArray("Hi!\r\n")), QStringLiteral("Hi!.."));
    QCOMPARE(HexUtils::printableAscii(QByteArray::fromHex("1f207e7f80")), QStringLiteral(". ~.."));
    QVERIFY(HexUtils::printableAscii(QByteArray()).isEmpty());
}

// ---------------------------------------------------------------------------------------

void Tst_hexutils::unescapeSimpleEscapes()
{
    QString error;
    QCOMPARE(HexUtils::unescape(QStringLiteral("a\\nb\\rc\\td"), &error), QByteArray("a\nb\rc\td"));
    QVERIFY(error.isEmpty());
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\0")), QByteArray(1, '\0'));
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\a\\b\\e\\f\\v")), QByteArray::fromHex("07081b0c0b"));
    QCOMPARE(HexUtils::unescape(QStringLiteral("x\\\\y")), QByteArray("x\\y"));
    QVERIFY(HexUtils::unescape(QString()).isEmpty());
    QCOMPARE(HexUtils::unescape(QStringLiteral("plain text")), QByteArray("plain text"));
}

void Tst_hexutils::unescapeHex()
{
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\x01\\xFF\\xab")), QByteArray::fromHex("01ffab"));
    // \x takes exactly two digits: "\x414" is 'A' followed by '4'.
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\x414")), QByteArray("A4"));
    // Raw bytes are not re-encoded as UTF-8.
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\xE4\\xB8\\xAD")), QByteArray::fromHex("e4b8ad"));
}

void Tst_hexutils::unescapeUnicode()
{
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\u0041")), QByteArray("A"));
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\u4E2D")), QString::fromUtf8("中").toUtf8());
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\u00e9")), QByteArray::fromHex("c3a9"));
}

void Tst_hexutils::unescapeSurrogatePair()
{
    // U+1F600 as two \u escapes must produce one 4-byte UTF-8 sequence.
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\uD83D\\uDE00")), QByteArray::fromHex("f09f9880"));
}

void Tst_hexutils::unescapePlainUtf8()
{
    const QString text = QString::fromUtf8("串口 test\\n");
    QCOMPARE(HexUtils::unescape(text), QString::fromUtf8("串口 test\n").toUtf8());
}

void Tst_hexutils::unescapeErrors()
{
    QString error;
    QVERIFY(HexUtils::unescape(QStringLiteral("abc\\q"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("\\q")));

    QVERIFY(HexUtils::unescape(QStringLiteral("abc\\"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("Trailing"), Qt::CaseInsensitive));

    QVERIFY(HexUtils::unescape(QStringLiteral("\\x4"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("\\x")));

    QVERIFY(HexUtils::unescape(QStringLiteral("\\xZZ"), &error).isEmpty());
    QVERIFY(!error.isEmpty());

    QVERIFY(HexUtils::unescape(QStringLiteral("\\u12"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("\\u")));

    QVERIFY(HexUtils::unescape(QStringLiteral("\\u12G4"), &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // Null error pointer is fine.
    QVERIFY(HexUtils::unescape(QStringLiteral("\\q"), nullptr).isEmpty());
}

void Tst_hexutils::unescapeClearsError()
{
    QString error = QStringLiteral("stale");
    QCOMPARE(HexUtils::unescape(QStringLiteral("ok\\n"), &error), QByteArray("ok\n"));
    QVERIFY(error.isEmpty());
}

void Tst_hexutils::unescapeWithEncoder()
{
    // Text runs (literals and \u escapes) go through the callback - here upper-cased Latin-1 -
    // while every \xHH byte is appended verbatim and never reaches the callback.
    int calls = 0;
    const auto upperLatin1 = [&calls](const QString& s) {
        ++calls;
        return s.toUpper().toLatin1();
    };
    QString error = QStringLiteral("stale");
    const QByteArray out = HexUtils::unescape(QStringLiteral("ab\\x80\\u00e9c"), upperLatin1, &error);
    QCOMPARE(out, QByteArray("AB") + QByteArray::fromHex("80") + QByteArray::fromHex("c9") + QByteArray("C"));
    QCOMPARE(calls, 2);   // "ab" flushed before \x80, "<e-acute>c" flushed at the end
    QVERIFY(error.isEmpty());

    // Only raw bytes: the callback is never invoked.
    calls = 0;
    QCOMPARE(HexUtils::unescape(QStringLiteral("\\xff\\x55"), upperLatin1), QByteArray::fromHex("ff55"));
    QCOMPARE(calls, 0);

    // The UTF-8 overload is the same parser with toUtf8() as the encoder.
    QCOMPARE(HexUtils::unescape(
                 QStringLiteral("\\u4E2D\\x00"), [](const QString& s) { return s.toUtf8(); }),
             HexUtils::unescape(QStringLiteral("\\u4E2D\\x00")));

    // The error path is unchanged: empty result, message set, callback output discarded.
    QVERIFY(HexUtils::unescape(QStringLiteral("abc\\q"), upperLatin1, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("\\q")));
    QVERIFY(HexUtils::unescape(QStringLiteral("\\x4"), upperLatin1, nullptr).isEmpty());
}

// ---------------------------------------------------------------------------------------

void Tst_hexutils::escapeControls()
{
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray("a\r\nb\tc")), QStringLiteral("a\\r\\nb\\tc"));
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("001b037f")), QStringLiteral("\\x00\\x1B\\x03\\x7F"));
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray("printable ~!")), QStringLiteral("printable ~!"));
    QVERIFY(HexUtils::escapeForDisplay(QByteArray()).isEmpty());
}

void Tst_hexutils::escapeUtf8()
{
    const QByteArray chinese = QString::fromUtf8("中文").toUtf8();
    QCOMPARE(HexUtils::escapeForDisplay(chinese), QString::fromUtf8("中文"));
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray("ok ") + chinese + QByteArray("\n")),
             QString::fromUtf8("ok 中文\\n"));
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("c3a9")), QString::fromUtf8("é"));
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("f09f9880")), QString::fromUcs4(U"\U0001F600"));
}

void Tst_hexutils::escapeInvalidUtf8()
{
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("ff")), QStringLiteral("\\xFF"));
    // Truncated 3-byte sequence.
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("e4b8")), QStringLiteral("\\xE4\\xB8"));
    // Lone continuation byte between ASCII.
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("4180 42")), QStringLiteral("A\\x80B"));
    // Overlong encoding of '/' must not be accepted.
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("c0af")), QStringLiteral("\\xC0\\xAF"));
    // Valid sequence followed by garbage.
    QCOMPARE(HexUtils::escapeForDisplay(QByteArray::fromHex("e4b8adfe")),
             QString::fromUtf8("中") + QStringLiteral("\\xFE"));
}

void Tst_hexutils::escapeRoundTrip()
{
    const QByteArray original =
        QByteArray("AT+GMR\r\n") + QByteArray::fromHex("031b") + QString::fromUtf8("串").toUtf8();
    const QString shown = HexUtils::escapeForDisplay(original);
    QCOMPARE(HexUtils::unescape(shown), original);
}

QTEST_GUILESS_MAIN(Tst_hexutils)
#include "tst_hexutils.moc"
