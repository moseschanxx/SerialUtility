// Unit tests for AnsiParser (src/terminal/AnsiParser.h): byte decoding, the VT500 state
// machine and the mapping of control sequences onto TerminalScreen operations.
#include <QtTest>
#include <QSignalSpy>

#include "terminal/AnsiParser.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalTypes.h"

#include <algorithm>

using namespace Terminal;
using namespace Qt::StringLiterals;

namespace {

/// A screen plus a parser feeding it. The default 5x20 grid keeps expectations short.
struct Term
{
    TerminalScreen screen;
    AnsiParser parser;

    explicit Term(int rows = 5, int cols = 20, int scrollback = 100)
        : screen(rows, cols, scrollback)
        , parser(&screen)
    {
    }

    void feed(const char* text) { parser.feed(QByteArray(text)); }
    void feed(const QByteArray& bytes) { parser.feed(bytes); }
    QString row(int r) const { return screen.line(r).text(); }
    const Cell& cell(int r, int c) const { return screen.line(r).cells[c]; }
    int cursorRow() const { return screen.cursor().row; }
    int cursorCol() const { return screen.cursor().col; }
};

const QChar kReplacement(char16_t(0xFFFD));

/// True when every character of `text` is U+FFFD (and there is at least one).
bool allReplacement(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar ch : text) {
        if (ch != kReplacement) {
            return false;
        }
    }
    return true;
}

} // namespace

class Tst_ansiparser : public QObject
{
    Q_OBJECT
private slots:
    void defaults();
    void plainText();
    void crlfAndBareLf();
    void lineFeedNewLineMode();
    void controlCharacters();
    void controlsInsideSequences();
    void sequenceSplitAcrossFeeds();
    void utf8SplitAcrossFeeds();
    void invalidUtf8();
    void truncatedUtf8AtChunkBoundary();
    void invalidUtf8ChunkEquivalence();
    void encodings();
    void sgrAttributes();
    void sgrColours();
    void sgrEdgeCases();
    void cursorMovement();
    void eraseSequences();
    void editSequences();
    void tabStops();
    void scrollRegion();
    void decPrivateModes();
    void alternateScreen();
    void insertMode();
    void dsrAndDaReplies();
    void oscTitle();
    void escSequences();
    void softReset();
    void alignmentPattern();
    void decSpecialGraphics();
    void unknownSequencesIgnored_data();
    void unknownSequencesIgnored();
    void garbageNeverDesyncs();
    void nonAsciiAbortsSequence_data();
    void nonAsciiAbortsSequence();
    void controlStringGarbageRecovers();
    void parserReset();
    void wideCharacters();
    void wrapAndScrollback();
    void bootLogSnippet();
    void busyboxLineEditing();
    void progressLines();
    void contentChangedBatching();
    void highVolumeOutput();
};

// ---- Basics -------------------------------------------------------------------------------

void Tst_ansiparser::defaults()
{
    Term t;
    QCOMPARE(t.parser.screen(), &t.screen);
    QVERIFY(t.parser.implicitCr());
    QCOMPARE(t.parser.encoding(), u"UTF-8"_s);
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QVERIFY(!t.parser.bracketedPasteMode());
    t.parser.setImplicitCr(false);
    QVERIFY(!t.parser.implicitCr());
    t.parser.setImplicitCr(true);
    QVERIFY(t.parser.implicitCr());

    t.feed(QByteArray()); // empty input is a no-op
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 0);
    QVERIFY(t.row(0).isEmpty());
}

void Tst_ansiparser::plainText()
{
    Term t;
    t.feed("hello");
    QCOMPARE(t.row(0), u"hello"_s);
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 5);
    t.feed(" world");
    QCOMPARE(t.row(0), u"hello world"_s);
    QCOMPARE(t.cursorCol(), 11);
    QCOMPARE(t.cell(0, 0).attr, Attributes());
    QCOMPARE(t.cell(0, 0).ch, U'h');
    QVERIFY(t.row(1).isEmpty());
}

void Tst_ansiparser::crlfAndBareLf()
{
    // implicitCr on (default): a bare LF also returns to column 0, CRLF is unaffected.
    Term t;
    t.feed("a\r\nb\nc");
    QCOMPARE(t.row(0), u"a"_s);
    QCOMPARE(t.row(1), u"b"_s);
    QCOMPARE(t.row(2), u"c"_s);
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 1);

    // implicitCr off: a bare LF keeps the column (classic staircase output).
    Term u;
    u.parser.setImplicitCr(false);
    u.feed("a\nb");
    QCOMPARE(u.row(0), u"a"_s);
    QCOMPARE(u.row(1), u" b"_s);
    QCOMPARE(u.cursorRow(), 1);
    QCOMPARE(u.cursorCol(), 2);
    u.feed("\r\nc");
    QCOMPARE(u.row(2), u"c"_s);
    QCOMPARE(u.cursorCol(), 1);

    // VT and FF behave like LF.
    Term v;
    v.feed("a\vb\fc");
    QCOMPARE(v.row(0), u"a"_s);
    QCOMPARE(v.row(1), u"b"_s);
    QCOMPARE(v.row(2), u"c"_s);
    QCOMPARE(v.cursorCol(), 1);

    // A lone CR returns to column 0 without a line feed.
    Term w;
    w.feed("abc\rX");
    QCOMPARE(w.row(0), u"Xbc"_s);
    QCOMPARE(w.cursorRow(), 0);
    QCOMPARE(w.cursorCol(), 1);
}

void Tst_ansiparser::lineFeedNewLineMode()
{
    Term t;
    t.parser.setImplicitCr(false);
    t.feed("\033[20h"); // LNM set: LF implies CR
    t.feed("ab\ncd");
    QCOMPARE(t.row(0), u"ab"_s);
    QCOMPARE(t.row(1), u"cd"_s);
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 2);
    t.feed("\033[20l\nef"); // LNM reset: LF keeps the column again
    QCOMPARE(t.row(2), u"  ef"_s);
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 4);
}

void Tst_ansiparser::controlCharacters()
{
    Term t;
    QSignalSpy bell(&t.screen, &TerminalScreen::bellRequested);
    t.feed("\007\007");
    QCOMPARE(bell.count(), 2);

    t.feed("ab\bX");
    QCOMPARE(t.row(0), u"aX"_s);
    QCOMPARE(t.cursorCol(), 2);

    // NUL, ENQ, XON, XOFF are ignored; SO/SI select G1/G0, and with both ASCII nothing changes.
    t.feed(QByteArray("\0\005\016\017\021\023", 6));
    QCOMPARE(t.row(0), u"aX"_s);
    QCOMPARE(t.cursorCol(), 2);

    // DEL is ignored.
    t.feed("\177Y");
    QCOMPARE(t.row(0), u"aXY"_s);

    // HT to the next tab stop.
    t.feed("\r\tZ");
    QCOMPARE(t.cell(0, 8).ch, U'Z');
    QCOMPARE(t.cursorCol(), 9);

    // BS stops at column 0 and never erases.
    t.feed("\r\b\bQ");
    QCOMPARE(t.row(0), u"QXY     Z"_s);
    QCOMPARE(t.cursorCol(), 1);
}

void Tst_ansiparser::controlsInsideSequences()
{
    Term t;
    // CAN and SUB abort the sequence; the collected parameters are discarded.
    t.feed("\033[31\030x");
    QCOMPARE(t.row(0), u"x"_s);
    QVERIFY(t.cell(0, 0).attr.fg.isDefault());
    t.feed("\033[31\032y");
    QCOMPARE(t.row(0), u"xy"_s);
    QVERIFY(t.cell(0, 1).attr.fg.isDefault());

    // ESC inside a CSI abandons it and starts a new sequence.
    t.feed("\033[31\033[32mz");
    QCOMPARE(t.row(0), u"xyz"_s);
    QCOMPARE(t.cell(0, 2).attr.fg, Color::indexed(2));

    // C0 controls inside a CSI are executed and the sequence continues (VT500 semantics).
    t.feed("\033[0m\r\n");
    t.feed("a\033[\n34mb");
    QCOMPARE(t.row(1), u"a"_s);
    QCOMPARE(t.row(2), u"b"_s);
    QCOMPARE(t.cell(2, 0).attr.fg, Color::indexed(4));

    // BEL inside a CSI rings the bell; the digits around it still form one parameter.
    QSignalSpy bell(&t.screen, &TerminalScreen::bellRequested);
    t.feed("\033[3\007" "1mq");
    QCOMPARE(bell.count(), 1);
    QCOMPARE(t.cell(2, 1).ch, U'q');
    QCOMPARE(t.cell(2, 1).attr.fg, Color::indexed(1));
}

// ---- Chunking and decoding ----------------------------------------------------------------

void Tst_ansiparser::sequenceSplitAcrossFeeds()
{
    Term t;
    t.feed("\033[3");
    t.feed("2m");
    t.feed("x");
    QCOMPARE(t.row(0), u"x"_s);
    QCOMPARE(t.cell(0, 0).attr.fg, Color::indexed(2));

    t.feed("\033");
    t.feed("[0m\033[");
    t.feed("2;");
    t.feed("3H");
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 2);

    t.feed("\033]0;Ti");
    t.feed("tle\007");
    QCOMPARE(t.screen.title(), u"Title"_s);

    t.feed("\033[");
    t.feed("?25");
    t.feed("l");
    QVERIFY(!t.screen.cursorVisible());

    // Every byte of a long sequence delivered on its own.
    const QByteArray seq = "\033[1;4;38;2;10;20;30;48;5;99mZ"_ba;
    for (const char ch : seq) {
        t.feed(QByteArray(1, ch));
    }
    const Cell& z = t.cell(1, 2);
    QCOMPARE(z.ch, U'Z');
    QVERIFY(z.attr.has(Bold));
    QVERIFY(z.attr.has(Underline));
    QCOMPARE(z.attr.fg, Color::rgb(10, 20, 30));
    QCOMPARE(z.attr.bg, Color::indexed(99));

    // A CSI cut off by non-ASCII text arriving in a later feed: the sequence is abandoned (the
    // collected "3" is never applied as SGR 3) and the text is printed.
    t.feed("\033[3");
    t.feed("\xE4\xB8\xAD");
    t.feed("ok");
    QVERIFY(t.row(1).endsWith(u"Z中ok"_s));
    QCOMPARE(t.cursorCol(), 7);
    QCOMPARE(t.screen.currentAttributes(), z.attr);
    QVERIFY(!t.screen.currentAttributes().has(Italic));
}

void Tst_ansiparser::utf8SplitAcrossFeeds()
{
    Term t;
    t.feed("\xE4");
    QCOMPARE(t.cursorCol(), 0);
    QVERIFY(t.row(0).isEmpty());
    t.feed("\xB8\xAD");
    QCOMPARE(t.cell(0, 0).ch, char32_t(0x4E2D));
    QVERIFY(t.cell(0, 0).attr.has(WideLead));
    QVERIFY(t.cell(0, 1).isWideTrail());
    QCOMPARE(t.cursorCol(), 2);

    // Four-byte sequences (a surrogate pair in UTF-16) split 1+3 and 3+1.
    t.feed("\xF0");
    t.feed("\x9F\x98\x80");
    QCOMPARE(t.cell(0, 2).ch, char32_t(0x1F600));
    QCOMPARE(t.cursorCol(), 4);
    t.feed("\xF0\x9F\x91");
    t.feed("\x8D");
    QCOMPARE(t.cell(0, 4).ch, char32_t(0x1F44D));
    QCOMPARE(t.cursorCol(), 6);

    // Split inside an OSC string as well.
    t.feed("\033]0;\xE6\x9D");
    t.feed("\xBF\007");
    QCOMPARE(t.screen.title(), u"板"_s);

    // Two-byte character split, one byte per feed, in the middle of text.
    t.feed("\r\nc");
    t.feed("\xC3");
    t.feed("\xA9");
    t.feed("!");
    QCOMPARE(t.row(1), u"cé!"_s);
}

void Tst_ansiparser::invalidUtf8()
{
    Term t(5, 40);
    t.feed("a\xFF" "b");
    const QString expected = u"a"_s + kReplacement + u"b"_s;
    QCOMPARE(t.row(0), expected);
    QCOMPARE(t.cursorCol(), 3);

    // Truncated multi-byte sequence followed by ASCII: replacement, then the ASCII.
    t.feed("\r\n\xE4\xB8x");
    const QString r1 = t.row(1);
    QVERIFY(r1.size() >= 2);
    QVERIFY(r1.endsWith(u'x'));
    QVERIFY(allReplacement(r1.left(r1.size() - 1)));
    QCOMPARE(t.cursorCol(), int(r1.size()));

    // Overlong encoding is rejected.
    t.feed("\r\n\xC0\xAF");
    QVERIFY(allReplacement(t.row(2)));

    // Stray 0x80-0x9F bytes are invalid UTF-8, not C1 controls: what follows is printed.
    t.feed("\r\n\x9B" "31mZ");
    const QString r3 = t.row(3);
    QVERIFY(r3.endsWith(u"31mZ"_s));
    QVERIFY(allReplacement(r3.left(r3.size() - 4)));
    QVERIFY(t.cell(3, int(r3.size()) - 1).attr.fg.isDefault());

    // Encoded surrogates (CESU-8) are invalid too.
    t.feed("\r\n\xED\xA0\x80!");
    const QString r4 = t.row(4);
    QVERIFY(r4.endsWith(u'!'));
    QVERIFY(allReplacement(r4.left(r4.size() - 1)));
}

void Tst_ansiparser::truncatedUtf8AtChunkBoundary()
{
    // A chunk ending inside an invalid multi-byte sequence must not swallow the bytes that
    // follow it in the next chunk (QStringDecoder on its own drops the first byte of the next
    // chunk, which on a serial console is typically the ESC of a control sequence).
    Term t(6, 40);
    t.feed("\xF0\x9F");
    t.feed("\033[31mR");
    const QString r0 = t.row(0);
    QVERIFY(r0.endsWith(u'R'));
    QVERIFY(allReplacement(r0.left(r0.size() - 1))); // one U+FFFD per broken byte, ESC preserved
    QCOMPARE(t.cell(0, int(r0.size()) - 1).attr.fg, Color::indexed(1));
    QCOMPARE(t.cursorCol(), int(r0.size()));

    // A lead byte followed by ASCII at the end of a chunk: the ASCII survives.
    t.feed("\033[0m\r\n\xE4" "A");
    t.feed("B");
    QCOMPARE(t.row(1), QString(kReplacement) + u"AB"_s);

    // A genuinely split character still decodes, also across an empty feed.
    t.feed("\r\n\xE4\xB8");
    t.feed(QByteArray());
    t.feed("\xADok");
    QCOMPARE(t.row(2), u"中ok"_s);

    // A valid-so-far prefix that is contradicted by the next chunk: replacement(s), then text.
    t.feed("\r\n\xE4\xB8");
    t.feed("x");
    const QString r3 = t.row(3);
    QVERIFY(r3.endsWith(u'x'));
    QVERIFY(allReplacement(r3.left(r3.size() - 1)));

    // Lone continuation bytes and an always-invalid lead at the chunk end.
    t.feed("\r\n\x80\xBF\xF5");
    QVERIFY(allReplacement(t.row(4)));
    QCOMPARE(t.row(4).size(), 3);

    // A truncated 4-byte sequence followed by RIS in the next chunk: the reset still happens.
    t.feed("\xF0\x9F\x98");
    t.feed("\033c");
    t.feed("ok");
    QCOMPARE(t.row(0), u"ok"_s);
    QVERIFY(t.row(4).isEmpty());

    // A four-byte character delivered one byte per feed.
    t.feed("\r\n");
    t.feed("\xF0");
    t.feed("\x9F");
    t.feed("\x98");
    t.feed("\x80");
    QCOMPARE(t.cell(1, 0).ch, char32_t(0x1F600));
    QCOMPARE(t.cursorCol(), 2);

    // A corrupted multi-byte character directly before a character that straddles the chunk
    // boundary: the decoder must not be handed the truncated prefix (it would swallow the
    // pending lead byte). Chunked and whole feeds must agree.
    struct Case
    {
        QByteArray a, b;
    } cases[] = {
        {"\x41\xE4\xB8\xC3", "\xA9\x42"},         // -> "A" U+FFFD U+FFFD e-acute "B"
        {"\xC3\xE4\xB8", "\xAD\x42"},             // -> U+FFFD U+4E2D "B"
        {"\xF0\x9F\x98\xF0", "\x9F\x98\x80\x42"}, // -> U+FFFD x3 U+1F600 "B"
        {"\xC2\xF0\xC1\xE0\x5A", ""},             // chain of cut-off leads: 4 x U+FFFD "Z"
        {"\x80\xEF\xF0\xC2\xE4\xED\xE0", "\x5A"}, // chain reaching back over several leads: 'Z' survives
        {"\xE4\xB8\xAD\xC3", "\xA9"},             // complete character right before the split stays intact
    };
    for (const Case& c : cases) {
        Term chunked(2, 40);
        Term whole(2, 40);
        chunked.feed(c.a);
        chunked.feed(c.b);
        whole.feed(c.a + c.b);
        QCOMPARE(chunked.row(0), whole.row(0));
        QCOMPARE(chunked.cursorCol(), whole.cursorCol());
    }
    {
        Term c0(2, 40);
        c0.feed(cases[0].a);
        c0.feed(cases[0].b);
        QCOMPARE(c0.row(0), u"A"_s + kReplacement + kReplacement + u"éB"_s);
        Term c1(2, 40);
        c1.feed(cases[1].a);
        c1.feed(cases[1].b);
        QCOMPARE(c1.row(0), QString(kReplacement) + u"中B"_s);
        Term c2(2, 40);
        c2.feed(cases[2].a);
        c2.feed(cases[2].b);
        QCOMPARE(c2.row(0), QString(3, kReplacement) + QString::fromUcs4(U"\U0001F600B"));
        Term c3(2, 40);
        c3.feed(cases[3].a);
        QCOMPARE(c3.row(0), QString(4, kReplacement) + u"Z"_s);
        Term c5(2, 40);
        c5.feed(cases[5].a);
        c5.feed(cases[5].b);
        QCOMPARE(c5.row(0), u"中é"_s);
    }
}

void Tst_ansiparser::invalidUtf8ChunkEquivalence()
{
    // Invalid UTF-8 directly before a character that straddles a chunk boundary: the screen
    // (text, cursor and every cell) must not depend on where the stream was cut. Each case is
    // fed whole, as the two given chunks, and one byte per feed. The whole-feed result is pinned
    // as well so the comparison cannot pass vacuously.
    struct Case
    {
        QByteArray a, b;
    };
    const Case cases[] = {
        {"\x41\xE4\xB8\xC3", "\xA9\x42"},         // "A" U+FFFD.. e-acute "B"
        {"\xC3\xE4\xB8", "\xAD\x42"},             // U+FFFD.. U+4E2D "B"
        {"\xF0\x9F\x98\xF0", "\x9F\x98\x80\x42"}, // U+FFFD.. U+1F600 "B"
    };
    const auto compare = [](const Term& chunked, const Term& whole) {
        QCOMPARE(chunked.row(0), whole.row(0));
        QCOMPARE(chunked.cursorCol(), whole.cursorCol());
        QCOMPARE(chunked.cursorRow(), whole.cursorRow());
        QVERIFY(chunked.screen.line(0).cells == whole.screen.line(0).cells);
    };
    for (const Case& c : cases) {
        const QByteArray all = c.a + c.b;
        Term whole(3, 20);
        whole.feed(all);

        Term chunked(3, 20);
        chunked.feed(c.a);
        chunked.feed(c.b);
        compare(chunked, whole);

        Term bytewise(3, 20);
        for (const char ch : all) {
            bytewise.feed(QByteArray(1, ch));
        }
        compare(bytewise, whole);
        if (QTest::currentTestFailed()) {
            return;
        }
    }

    // Pin the whole-feed results.
    {
        Term w(3, 20);
        w.feed(cases[0].a + cases[0].b);
        const QString r = w.row(0);
        QVERIFY(r.startsWith(u'A'));
        QVERIFY(r.endsWith(u"éB"_s));
        QVERIFY(allReplacement(r.mid(1, r.size() - 3)));
    }
    {
        Term w(3, 20);
        w.feed(cases[1].a + cases[1].b);
        const QString r = w.row(0);
        QVERIFY(r.endsWith(u"中B"_s));
        QVERIFY(allReplacement(r.left(r.size() - 2)));
    }
    {
        Term w(3, 20);
        w.feed(cases[2].a + cases[2].b);
        const QString r = w.row(0);
        QVERIFY(r.endsWith(u'B'));
        // The replacement run occupies one cell each; the emoji follows it as a wide character.
        int k = 0;
        while (k < 20 && w.cell(0, k).ch == char32_t(0xFFFD)) {
            ++k;
        }
        QVERIFY(k >= 1);
        QCOMPARE(w.cell(0, k).ch, char32_t(0x1F600));
        QVERIFY(w.cell(0, k).attr.has(WideLead));
        QVERIFY(w.cell(0, k + 1).isWideTrail());
        QCOMPARE(w.cell(0, k + 2).ch, U'B');
        QCOMPARE(w.cursorCol(), k + 3);
    }
}

void Tst_ansiparser::encodings()
{
    const QStringList list = AnsiParser::availableEncodings();
    QCOMPARE(list.first(), u"UTF-8"_s);
    QVERIFY(list.contains(u"ISO-8859-1"_s));
    QVERIFY(list.contains(u"System"_s));

    Term t(4, 20);
    QVERIFY(t.parser.setEncoding(u"ISO-8859-1"_s));
    QCOMPARE(t.parser.encoding(), u"ISO-8859-1"_s);
    t.feed("caf\xE9");
    QCOMPARE(t.row(0), u"café"_s);

    // In an 8-bit stream the C1 controls are real controls: CSI, OSC and ST.
    t.feed("\r\n\x9B" "31mR");
    QCOMPARE(t.row(1), u"R"_s);
    QCOMPARE(t.cell(1, 0).attr.fg, Color::indexed(1));
    t.feed("\x9D" "0;Latin\x9C");
    QCOMPARE(t.screen.title(), u"Latin"_s);
    t.feed("\033[0m");

    // Unknown names fall back to UTF-8 and report failure.
    QVERIFY(!t.parser.setEncoding(u"no-such-codec"_s));
    QCOMPARE(t.parser.encoding(), u"UTF-8"_s);
    t.feed("\r\n\xE4\xB8\xAD");
    QCOMPARE(t.row(2), u"中"_s);

    QVERIFY(t.parser.setEncoding(u"System"_s));
    QCOMPARE(t.parser.encoding(), u"System"_s);
    QVERIFY(t.parser.setEncoding(u"utf-8"_s)); // case-insensitive names are accepted as given
    QCOMPARE(t.parser.encoding(), u"utf-8"_s);
    t.feed("\r\033[K\xE6\x96\x87");
    QCOMPARE(t.row(2), u"文"_s);

    if (list.contains(u"GB18030"_s)) {
        QVERIFY(t.parser.setEncoding(u"GB18030"_s));
        t.feed("\r\033[K\xD6\xD0");
        QCOMPARE(t.row(2), u"中"_s);
    }

    // Changing the encoding drops a pending partial sequence instead of corrupting the
    // following text.
    QVERIFY(t.parser.setEncoding(u"UTF-8"_s));
    t.feed("\r\n\xE4");
    QVERIFY(t.parser.setEncoding(u"UTF-8"_s));
    t.feed("x");
    QCOMPARE(t.row(3), u"x"_s);
}

// ---- SGR ----------------------------------------------------------------------------------

void Tst_ansiparser::sgrAttributes()
{
    Term t(3, 40);
    t.feed("\033[1;3;4;5;7;8;9mA");
    const Attributes a = t.cell(0, 0).attr;
    QVERIFY(a.has(Bold));
    QVERIFY(a.has(Italic));
    QVERIFY(a.has(Underline));
    QVERIFY(a.has(Blink));
    QVERIFY(a.has(Inverse));
    QVERIFY(a.has(Hidden));
    QVERIFY(a.has(Strike));
    QVERIFY(!a.has(Dim));
    QVERIFY(!a.has(WideLead));

    t.feed("\033[2mB");
    QVERIFY(t.cell(0, 1).attr.has(Dim));
    QVERIFY(t.cell(0, 1).attr.has(Bold));
    t.feed("\033[22mC"); // bold and dim off together
    QVERIFY(!t.cell(0, 2).attr.has(Bold));
    QVERIFY(!t.cell(0, 2).attr.has(Dim));
    QVERIFY(t.cell(0, 2).attr.has(Italic));
    t.feed("\033[23;24;25;27;28;29mD");
    QCOMPARE(t.cell(0, 3).attr.flags, quint16(NoAttr));
    t.feed("\033[21mE"); // double underline renders as underline
    QCOMPARE(t.cell(0, 4).attr.flags, quint16(Underline));
    t.feed("\033[24;6mF"); // rapid blink
    QCOMPARE(t.cell(0, 5).attr.flags, quint16(Blink));
    t.feed("\033[0mG");
    QCOMPARE(t.cell(0, 6).attr, Attributes());
    t.feed("\033[1mH\033[mI"); // SGR with no parameters is SGR 0
    QVERIFY(t.cell(0, 7).attr.has(Bold));
    QCOMPARE(t.cell(0, 8).attr, Attributes());

    // Underline styles: 4:0 off, 4:1..4:5 on, plain 4 on.
    t.feed("\033[4:3mJ\033[4:0mK\033[4:1mL\033[24m\033[4mM");
    QVERIFY(t.cell(0, 9).attr.has(Underline));
    QVERIFY(!t.cell(0, 10).attr.has(Underline));
    QVERIFY(t.cell(0, 11).attr.has(Underline));
    QVERIFY(t.cell(0, 12).attr.has(Underline));

    // Attributes persist across lines and control characters.
    t.feed("\033[0;1m\r\nN");
    QVERIFY(t.cell(1, 0).attr.has(Bold));
    QCOMPARE(t.screen.currentAttributes().flags, quint16(Bold));
}

void Tst_ansiparser::sgrColours()
{
    Term t(3, 40);
    t.feed("\033[31;42mA");
    QCOMPARE(t.cell(0, 0).attr.fg, Color::indexed(1));
    QCOMPARE(t.cell(0, 0).attr.bg, Color::indexed(2));
    t.feed("\033[39mB");
    QVERIFY(t.cell(0, 1).attr.fg.isDefault());
    QCOMPARE(t.cell(0, 1).attr.bg, Color::indexed(2));
    t.feed("\033[49mC");
    QVERIFY(t.cell(0, 2).attr.bg.isDefault());
    t.feed("\033[97;100mD");
    QCOMPARE(t.cell(0, 3).attr.fg, Color::indexed(15));
    QCOMPARE(t.cell(0, 3).attr.bg, Color::indexed(8));
    t.feed("\033[0;90mE\033[107mF");
    QCOMPARE(t.cell(0, 4).attr.fg, Color::indexed(8));
    QCOMPARE(t.cell(0, 5).attr.bg, Color::indexed(15));

    // 256-colour and truecolour, semicolon form.
    t.feed("\033[0;38;5;208mG");
    QCOMPARE(t.cell(0, 6).attr.fg, Color::indexed(208));
    t.feed("\033[48;5;17mH");
    QCOMPARE(t.cell(0, 7).attr.bg, Color::indexed(17));
    t.feed("\033[38;2;10;20;30mI");
    QCOMPARE(t.cell(0, 8).attr.fg, Color::rgb(10, 20, 30));
    t.feed("\033[48;2;1;2;3mJ");
    QCOMPARE(t.cell(0, 9).attr.bg, Color::rgb(1, 2, 3));

    // Colon form, including the malformed variant without the colour-space id.
    t.feed("\033[0;38:5:100mK");
    QCOMPARE(t.cell(0, 10).attr.fg, Color::indexed(100));
    t.feed("\033[38:2::7:8:9mL");
    QCOMPARE(t.cell(0, 11).attr.fg, Color::rgb(7, 8, 9));
    t.feed("\033[48:2:250:251:252mM");
    QCOMPARE(t.cell(0, 12).attr.bg, Color::rgb(250, 251, 252));

    // Mixed in one sequence.
    t.feed("\033[0;1;38;2;255;0;0;48;5;4;4mN");
    QCOMPARE(t.cell(0, 13).attr.fg, Color::rgb(255, 0, 0));
    QCOMPARE(t.cell(0, 13).attr.bg, Color::indexed(4));
    QCOMPARE(t.cell(0, 13).attr.flags, quint16(Bold | Underline));
    t.feed("\033[0;38:2::1:2:3;48:5:200;3mO");
    QCOMPARE(t.cell(0, 14).attr.fg, Color::rgb(1, 2, 3));
    QCOMPARE(t.cell(0, 14).attr.bg, Color::indexed(200));
    QCOMPARE(t.cell(0, 14).attr.flags, quint16(Italic));

    // Out-of-range components are clamped.
    t.feed("\033[0;38;2;300;400;500mP");
    QCOMPARE(t.cell(0, 15).attr.fg, Color::rgb(255, 255, 255));
    t.feed("\033[38;5;999mQ");
    QCOMPARE(t.cell(0, 16).attr.fg, Color::indexed(255));

    // Underline colour (58/59) is parsed and ignored without eating the parameters after it.
    t.feed("\033[0;58;2;1;2;3;1mR");
    QVERIFY(t.cell(0, 17).attr.fg.isDefault());
    QVERIFY(t.cell(0, 17).attr.has(Bold));
    t.feed("\033[58:5:1;59;31mS");
    QCOMPARE(t.cell(0, 18).attr.fg, Color::indexed(1));
    QVERIFY(t.cell(0, 18).attr.has(Bold));

    // The screen's SGR state matches the last cell written.
    QCOMPARE(t.screen.currentAttributes(), t.cell(0, 18).attr);
}

void Tst_ansiparser::sgrEdgeCases()
{
    Term t(3, 40);
    t.feed("\033[;1mA"); // leading empty parameter is 0 (reset)
    QCOMPARE(t.cell(0, 0).attr.flags, quint16(Bold));
    QVERIFY(t.cell(0, 0).attr.fg.isDefault());
    t.feed("\033[31;mB"); // trailing empty parameter resets
    QCOMPARE(t.cell(0, 1).attr, Attributes());
    t.feed("\033[0;1;;4mC"); // empty middle parameter resets too
    QCOMPARE(t.cell(0, 2).attr.flags, quint16(Underline));
    t.feed("\033[0;1;38mD"); // 38 without a colour kind: ignored
    QCOMPARE(t.cell(0, 3).attr.flags, quint16(Bold));
    QVERIFY(t.cell(0, 3).attr.fg.isDefault());
    t.feed("\033[0;38;9;5;1mE"); // unknown colour kind: skipped, following parameters apply
    QCOMPARE(t.cell(0, 4).attr.flags, quint16(Blink | Bold));
    QVERIFY(t.cell(0, 4).attr.fg.isDefault());
    t.feed("\033[0;38;5mF"); // truncated 256-colour: the missing index reads as 0
    QCOMPARE(t.cell(0, 5).attr.fg, Color::indexed(0));
    t.feed("\033[0;99999mG"); // capped at 65535, unknown, ignored
    QCOMPARE(t.cell(0, 6).attr, Attributes());
    t.feed("\033[0;38:2mH"); // colon form without components: ignored
    QVERIFY(t.cell(0, 7).attr.fg.isDefault());

    // More than 32 parameters: the first 32 are honoured, the rest ignored.
    QByteArray many = "\033[0"_ba;
    for (int i = 0; i < 31; ++i) {
        many += ";1";
    }
    many += ";31mI"; // 33rd parameter
    t.feed(many);
    QCOMPARE(t.cell(0, 8).attr.flags, quint16(Bold));
    QVERIFY(t.cell(0, 8).attr.fg.isDefault());

    // Too many sub-parameters do not corrupt the sequence or the text after it.
    t.feed("\033[0;1:2:3:4:5:6:7:8:9:10:11:12mJ");
    QCOMPARE(t.cell(0, 9).ch, U'J');
    QCOMPARE(t.cursorCol(), 10);

    // Sub-parameter overflow followed by ';' must not synthesize a 0 (SGR reset): the
    // parameters collected before the overflow stay in effect, the rest is ignored.
    t.feed("\033[0mK\033[1;4:1:2:3:4:5:6:7:8:9;31mX");
    QCOMPARE(t.cell(0, 11).ch, U'X');
    QCOMPARE(t.cell(0, 11).attr.flags, quint16(Bold | Underline));
    QVERIFY(t.cell(0, 11).attr.fg.isDefault()); // "31" after the overflow is dropped, not applied
    QCOMPARE(t.cursorCol(), 12);
}

// ---- Cursor / erase / edit ----------------------------------------------------------------

void Tst_ansiparser::cursorMovement()
{
    Term t(10, 20);
    t.feed("\033[5;10H");
    QCOMPARE(t.cursorRow(), 4);
    QCOMPARE(t.cursorCol(), 9);
    t.feed("\033[A");
    QCOMPARE(t.cursorRow(), 3);
    t.feed("\033[3A");
    QCOMPARE(t.cursorRow(), 0);
    t.feed("\033[A"); // clamped at the top
    QCOMPARE(t.cursorRow(), 0);
    t.feed("\033[2B");
    QCOMPARE(t.cursorRow(), 2);
    t.feed("\033[C");
    QCOMPARE(t.cursorCol(), 10);
    t.feed("\033[5D");
    QCOMPARE(t.cursorCol(), 5);
    t.feed("\033[99C");
    QCOMPARE(t.cursorCol(), 19);
    t.feed("\033[99D");
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[99B");
    QCOMPARE(t.cursorRow(), 9);
    t.feed("\033[0A"); // 0 means 1
    QCOMPARE(t.cursorRow(), 8);

    t.feed("\033[H");
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[;5H"); // missing row defaults to 1
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 4);
    t.feed("\033[3;f"); // HVP
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[99;99H");
    QCOMPARE(t.cursorRow(), 9);
    QCOMPARE(t.cursorCol(), 19);

    t.feed("\033[7G"); // CHA
    QCOMPARE(t.cursorCol(), 6);
    t.feed("\033[4d"); // VPA
    QCOMPARE(t.cursorRow(), 3);
    t.feed("\033[2E"); // CNL
    QCOMPARE(t.cursorRow(), 5);
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[3G\033[E");
    QCOMPARE(t.cursorRow(), 6);
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[5G\033[3F"); // CPL
    QCOMPARE(t.cursorRow(), 3);
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[3`"); // HPA
    QCOMPARE(t.cursorCol(), 2);
    t.feed("\033[2a"); // HPR
    QCOMPARE(t.cursorCol(), 4);
    t.feed("\033[2e"); // VPR
    QCOMPARE(t.cursorRow(), 5);

    // Cursor movement clears a pending wrap: the next character overwrites the last column.
    t.feed("\033[1;1H");
    t.feed(QByteArray(20, 'a'));
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 19);
    t.feed("\033[Db");
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cell(0, 18).ch, U'b');
    QCOMPARE(t.row(1), u""_s);

    // SCOSC / SCORC
    t.feed("\033[7;7H\033[s\033[1;1H\033[u");
    QCOMPARE(t.cursorRow(), 6);
    QCOMPARE(t.cursorCol(), 6);

    // CUU from below a scroll region stops at its top margin; CUD from above stops at its
    // bottom margin (xterm CursorUp/CursorDown).
    Term u(24, 20);
    u.feed("\033[3;6r\033[8;1H\033[20A");
    QCOMPARE(u.cursorRow(), 2);
    u.feed("\033[1;1H\033[20B");
    QCOMPARE(u.cursorRow(), 5);
}

void Tst_ansiparser::eraseSequences()
{
    Term t(3, 5);
    const auto fill = [&t]() { t.feed("\033[0m\033[2J\033[Habcde\r\nfghij\r\nklmno"); };

    fill();
    t.feed("\033[2;3H\033[K");
    QCOMPARE(t.row(0), u"abcde"_s);
    QCOMPARE(t.row(1), u"fg"_s);
    QCOMPARE(t.row(2), u"klmno"_s);
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 2);

    fill();
    t.feed("\033[2;3H\033[0K");
    QCOMPARE(t.row(1), u"fg"_s);
    fill();
    t.feed("\033[2;3H\033[1K");
    QCOMPARE(t.row(1), u"   ij"_s);
    fill();
    t.feed("\033[2;3H\033[2K");
    QVERIFY(t.row(1).isEmpty());
    QCOMPARE(t.row(0), u"abcde"_s);
    QCOMPARE(t.row(2), u"klmno"_s);

    fill();
    t.feed("\033[2;3H\033[J");
    QCOMPARE(t.row(0), u"abcde"_s);
    QCOMPARE(t.row(1), u"fg"_s);
    QVERIFY(t.row(2).isEmpty());
    fill();
    t.feed("\033[2;3H\033[1J");
    QVERIFY(t.row(0).isEmpty());
    QCOMPARE(t.row(1), u"   ij"_s);
    QCOMPARE(t.row(2), u"klmno"_s);
    fill();
    t.feed("\033[2;3H\033[2J");
    QVERIFY(t.row(0).isEmpty());
    QVERIFY(t.row(1).isEmpty());
    QVERIFY(t.row(2).isEmpty());
    QCOMPARE(t.cursorRow(), 1); // ED does not move the cursor
    QCOMPARE(t.cursorCol(), 2);

    // DECSED / DECSEL behave like ED / EL.
    fill();
    t.feed("\033[2;3H\033[?K");
    QCOMPARE(t.row(1), u"fg"_s);
    t.feed("\033[?2J");
    QVERIFY(t.row(0).isEmpty());
    QVERIFY(t.row(2).isEmpty());

    // Erased cells carry the current background colour only.
    fill();
    t.feed("\033[1;44m\033[2;1H\033[2K");
    for (int c = 0; c < 5; ++c) {
        QCOMPARE(t.cell(1, c).ch, U' ');
        QCOMPARE(t.cell(1, c).attr.bg, Color::indexed(4));
        QVERIFY(t.cell(1, c).attr.fg.isDefault());
        QCOMPARE(t.cell(1, c).attr.flags, quint16(NoAttr));
    }

    // ED 3 clears the scrollback and nothing else.
    Term u(2, 5, 10);
    u.feed("a\r\nb\r\nc");
    QCOMPARE(u.screen.scrollbackSize(), 1);
    u.feed("\033[3J");
    QCOMPARE(u.screen.scrollbackSize(), 0);
    QCOMPARE(u.row(0), u"b"_s);
    QCOMPARE(u.row(1), u"c"_s);
}

void Tst_ansiparser::editSequences()
{
    Term t(4, 10);
    t.feed("abcde\033[2G\033[2@"); // ICH
    QCOMPARE(t.row(0), u"a  bcde"_s);
    QCOMPARE(t.cursorCol(), 1);
    t.feed("\033[P"); // DCH
    QCOMPARE(t.row(0), u"a bcde"_s);
    t.feed("\033[2X"); // ECH
    QCOMPARE(t.row(0), u"a  cde"_s);
    QCOMPARE(t.cursorCol(), 1);
    t.feed("\033[2;1Hz\033[3b"); // REP
    QCOMPARE(t.row(1), u"zzzz"_s);
    QCOMPARE(t.cursorCol(), 4);

    t.feed("\033[3;1HT");
    t.feed("\033[2;1H\033[L"); // IL
    QCOMPARE(t.row(0), u"a  cde"_s);
    QVERIFY(t.row(1).isEmpty());
    QCOMPARE(t.row(2), u"zzzz"_s);
    QCOMPARE(t.row(3), u"T"_s);
    t.feed("\033[M"); // DL
    QCOMPARE(t.row(1), u"zzzz"_s);
    QCOMPARE(t.row(2), u"T"_s);
    QVERIFY(t.row(3).isEmpty());
    t.feed("\033[2M");
    QCOMPARE(t.row(0), u"a  cde"_s);
    QVERIFY(t.row(1).isEmpty());
    QVERIFY(t.row(2).isEmpty());
    QVERIFY(t.row(3).isEmpty());

    t.feed("\033[2;1Hq\033[S"); // SU
    QCOMPARE(t.row(0), u"q"_s);
    QVERIFY(t.row(1).isEmpty());
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 1);
    t.feed("\033[2T"); // SD
    QVERIFY(t.row(0).isEmpty());
    QVERIFY(t.row(1).isEmpty());
    QCOMPARE(t.row(2), u"q"_s);
    QCOMPARE(t.screen.scrollbackSize(), 0); // editing operations never push to the scrollback
}

void Tst_ansiparser::tabStops()
{
    Term t(3, 40);
    t.feed("\tX");
    QCOMPARE(t.cell(0, 8).ch, U'X');
    QCOMPARE(t.cursorCol(), 9);
    t.feed("\033[3G\033H\033[1G\tY"); // HTS at column 3
    QCOMPARE(t.cell(0, 2).ch, U'Y');
    QCOMPARE(t.cursorCol(), 3);
    t.feed("\033[9G\033[g\033[1G\t\tW"); // TBC 0 clears the stop at column 9
    QCOMPARE(t.cell(0, 16).ch, U'W');
    QCOMPARE(t.cursorCol(), 17);
    t.feed("\033[3g\r\tZ"); // TBC 3 clears all: tab goes to the last column
    QCOMPARE(t.cell(0, 39).ch, U'Z');

    Term u(3, 40);
    u.feed("\033[2I"); // CHT
    QCOMPARE(u.cursorCol(), 16);
    u.feed("\033[Z"); // CBT
    QCOMPARE(u.cursorCol(), 8);
    u.feed("\033[2Z");
    QCOMPARE(u.cursorCol(), 0);
    u.feed("\033[5G\033[Z");
    QCOMPARE(u.cursorCol(), 0);
    u.feed("\033[18G\033[Z");
    QCOMPARE(u.cursorCol(), 16);
    u.feed("\033[I");
    QCOMPARE(u.cursorCol(), 24);
}

void Tst_ansiparser::scrollRegion()
{
    Term t(5, 10, 10);
    t.feed("\033[2;4r");
    QCOMPARE(t.screen.scrollTop(), 1);
    QCOMPARE(t.screen.scrollBottom(), 3);
    QCOMPARE(t.cursorRow(), 0); // DECSTBM homes the cursor
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033[1;1H0\033[2;1H1\033[3;1H2\033[4;1H3\033[5;1H4");
    t.feed("\033[4;1H\n"); // LF on the bottom margin scrolls the region only
    QCOMPARE(t.row(0), u"0"_s);
    QCOMPARE(t.row(1), u"2"_s);
    QCOMPARE(t.row(2), u"3"_s);
    QVERIFY(t.row(3).isEmpty());
    QCOMPARE(t.row(4), u"4"_s);
    QCOMPARE(t.screen.scrollbackSize(), 0);
    QCOMPARE(t.cursorRow(), 3);
    t.feed("\033[2;1H\033M"); // RI on the top margin scrolls the region down
    QCOMPARE(t.row(0), u"0"_s);
    QVERIFY(t.row(1).isEmpty());
    QCOMPARE(t.row(2), u"2"_s);
    QCOMPARE(t.row(3), u"3"_s);
    QCOMPARE(t.row(4), u"4"_s);

    t.feed("\033[4;2r"); // invalid: ignored
    QCOMPARE(t.screen.scrollTop(), 1);
    QCOMPARE(t.screen.scrollBottom(), 3);
    t.feed("\033[;3r"); // only the bottom given
    QCOMPARE(t.screen.scrollTop(), 0);
    QCOMPARE(t.screen.scrollBottom(), 2);
    t.feed("\033[r"); // reset to the full screen
    QCOMPARE(t.screen.scrollTop(), 0);
    QCOMPARE(t.screen.scrollBottom(), 4);
    QCOMPARE(t.cursorRow(), 0);
    t.feed("\033[5;1H\n"); // full-screen scroll pushes into the scrollback
    QCOMPARE(t.screen.scrollbackSize(), 1);
    QCOMPARE(t.screen.scrollbackLine(0).text(), u"0"_s);
    QCOMPARE(t.row(3), u"4"_s);
    QVERIFY(t.row(4).isEmpty());

    // Origin mode: addresses are relative to the region and clamped to it.
    t.feed("\033[2;4r\033[?6h");
    QVERIFY(t.screen.originMode());
    QCOMPARE(t.cursorRow(), 1);
    t.feed("\033[1;1HX");
    QCOMPARE(t.cell(1, 0).ch, U'X');
    t.feed("\033[99;1HY");
    QCOMPARE(t.cell(3, 0).ch, U'Y');
    t.feed("\033[?6l\033[r");
    QVERIFY(!t.screen.originMode());
}

// ---- Modes --------------------------------------------------------------------------------

void Tst_ansiparser::decPrivateModes()
{
    Term t(5, 10);
    QSignalSpy keyMode(&t.parser, &AnsiParser::cursorKeyModeChanged);
    QSignalSpy paste(&t.parser, &AnsiParser::bracketedPasteChanged);

    t.feed("\033[?25l");
    QVERIFY(!t.screen.cursorVisible());
    t.feed("\033[?25h");
    QVERIFY(t.screen.cursorVisible());

    t.feed("\033[?7l");
    QVERIFY(!t.screen.autoWrap());
    t.feed("abcdefghijkl");
    QCOMPARE(t.row(0), u"abcdefghil"_s);
    QCOMPARE(t.cursorRow(), 0);
    t.feed("\033[?7h");
    QVERIFY(t.screen.autoWrap());

    t.feed("\033[?1h");
    QVERIFY(t.parser.cursorKeyApplicationMode());
    QCOMPARE(keyMode.count(), 1);
    QCOMPARE(keyMode.last().at(0).toBool(), true);
    t.feed("\033[?1h"); // unchanged: no second signal
    QCOMPARE(keyMode.count(), 1);
    t.feed("\033[?1l");
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QCOMPARE(keyMode.count(), 2);
    QCOMPARE(keyMode.last().at(0).toBool(), false);

    t.feed("\033[?2004h");
    QVERIFY(t.parser.bracketedPasteMode());
    QCOMPARE(paste.count(), 1);
    QCOMPARE(paste.last().at(0).toBool(), true);
    t.feed("\033[?2004l");
    QVERIFY(!t.parser.bracketedPasteMode());
    QCOMPARE(paste.count(), 2);
    QCOMPARE(paste.last().at(0).toBool(), false);

    t.feed("\033[?6h");
    QVERIFY(t.screen.originMode());
    t.feed("\033[?6l");
    QVERIFY(!t.screen.originMode());

    t.feed("\033[4h");
    QVERIFY(t.screen.insertMode());
    t.feed("\033[4l");
    QVERIFY(!t.screen.insertMode());

    // Several modes in one sequence.
    t.feed("\033[?25;7l");
    QVERIFY(!t.screen.cursorVisible());
    QVERIFY(!t.screen.autoWrap());
    t.feed("\033[?25;7h");
    QVERIFY(t.screen.cursorVisible());
    QVERIFY(t.screen.autoWrap());

    // Modes we consume and ignore must not eat the following text or change state.
    t.feed("\033[2J\033[H\033[?12h\033[?1000h\033[?1002h\033[?1003h\033[?1004h\033[?1005h\033[?1006h"
           "\033[?7727h\033[?8452h\033[?9999h\033[2h\033[12h\033[?1000l\033[?12lok");
    QCOMPARE(t.row(0), u"ok"_s);
    QVERIFY(t.screen.cursorVisible());
    QVERIFY(t.screen.autoWrap());
    QVERIFY(!t.screen.insertMode());
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QVERIFY(!t.parser.bracketedPasteMode());
    QCOMPARE(keyMode.count(), 2);
    QCOMPARE(paste.count(), 2);
}

void Tst_ansiparser::alternateScreen()
{
    Term t(3, 10, 10);
    t.feed("primary");
    t.feed("\033[?1049h");
    QVERIFY(t.screen.alternateScreenActive());
    QVERIFY(t.row(0).isEmpty());
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 7);
    t.feed("\033[Halt\r\n\r\n\r\n\r\n");
    QCOMPARE(t.screen.scrollbackSize(), 0); // no scrollback while the alternate screen is active
    t.feed("\033[?1049l");
    QVERIFY(!t.screen.alternateScreenActive());
    QCOMPARE(t.row(0), u"primary"_s);
    QCOMPARE(t.cursorRow(), 0); // 1049 restores the cursor
    QCOMPARE(t.cursorCol(), 7);

    // Redundant ?1049l on the primary screen still restores the saved cursor (xterm).
    t.feed("\033[2;3H\033[?1049h\033[?1049l\033[4;5H\033[?1049l");
    QVERIFY(!t.screen.alternateScreenActive());
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 2);
    QCOMPARE(t.row(0), u"primary"_s); // primary contents untouched

    // Redundant ?1049h while already on the alternate screen saves the cursor
    // (alternate slot) and clears the stale contents without moving the cursor.
    t.feed("\033[?1049h\033[Hstale\033[2;4H\033[?1049h");
    QVERIFY(t.screen.alternateScreenActive());
    QVERIFY(t.row(0).isEmpty());
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 3);
    t.feed("\033[3;1H\033[?1048l"); // DECRC-equivalent restores the alt-slot save
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 3);
    t.feed("\033[?1049l");
    QVERIFY(!t.screen.alternateScreenActive());
    QCOMPARE(t.row(0), u"primary"_s);
    QCOMPARE(t.cursorRow(), 1); // primary slot saved by the first ?1049h of this block
    QCOMPARE(t.cursorCol(), 2);
    t.feed("\033[1;8H"); // back to where the first ?1049l left the cursor for the ?47 checks

    t.feed("\033[?47h");
    QVERIFY(t.screen.alternateScreenActive());
    t.feed("x");
    t.feed("\033[?47l");
    QVERIFY(!t.screen.alternateScreenActive());
    QCOMPARE(t.row(0), u"primary"_s);
    QCOMPARE(t.cursorCol(), 8); // 47 does not touch the cursor

    t.feed("\033[?1047h");
    QVERIFY(t.screen.alternateScreenActive());
    QVERIFY(t.row(0).isEmpty());
    t.feed("\033[?1047l");
    QCOMPARE(t.row(0), u"primary"_s);

    // 1048 saves / restores the cursor without switching screens.
    t.feed("\033[1;2H\033[?1048h\033[3;3H\033[?1048l");
    QVERIFY(!t.screen.alternateScreenActive());
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 1);
}

void Tst_ansiparser::insertMode()
{
    Term t(3, 10);
    t.feed("abc\033[4h\033[1GXY\033[4l");
    QCOMPARE(t.row(0), u"XYabc"_s);
    QCOMPARE(t.cursorCol(), 2);
    t.feed("\033[1GQ");
    QCOMPARE(t.row(0), u"QYabc"_s);
}

// ---- Replies ------------------------------------------------------------------------------

void Tst_ansiparser::dsrAndDaReplies()
{
    Term t(10, 20);
    QSignalSpy resp(&t.parser, &AnsiParser::responseRequested);

    t.feed("\033[3;7H\033[6n");
    QCOMPARE(resp.count(), 1);
    QCOMPARE(resp.at(0).at(0).toByteArray(), "\033[3;7R"_ba);

    t.feed("\033[5n");
    QCOMPARE(resp.count(), 2);
    QCOMPARE(resp.at(1).at(0).toByteArray(), "\033[0n"_ba);

    t.feed("\033[c");
    QCOMPARE(resp.count(), 3);
    QCOMPARE(resp.at(2).at(0).toByteArray(), "\033[?1;2c"_ba);
    t.feed("\033[0c");
    QCOMPARE(resp.count(), 4);
    QCOMPARE(resp.at(3).at(0).toByteArray(), "\033[?1;2c"_ba);
    t.feed("\033Z"); // DECID answers like DA
    QCOMPARE(resp.count(), 5);
    QCOMPARE(resp.at(4).at(0).toByteArray(), "\033[?1;2c"_ba);
    t.feed("\033[1c"); // DA with a non-zero parameter: no reply
    QCOMPARE(resp.count(), 5);

    t.feed("\033[?6n"); // DECXCPR
    QCOMPARE(resp.count(), 6);
    QCOMPARE(resp.at(5).at(0).toByteArray(), "\033[?3;7R"_ba);

    // In origin mode the reported row is relative to the scroll region.
    t.feed("\033[3;8r\033[?6h\033[2;1H\033[6n");
    QCOMPARE(resp.count(), 7);
    QCOMPARE(resp.at(6).at(0).toByteArray(), "\033[2;1R"_ba);
    QCOMPARE(t.cursorRow(), 3);
    t.feed("\033[?6l\033[r");

    // Position after the cursor has been at the last column (pending wrap) is still 1-based.
    t.feed("\033[10;20H\033[6n");
    QCOMPARE(resp.count(), 8);
    QCOMPARE(resp.at(7).at(0).toByteArray(), "\033[10;20R"_ba);

    // Nothing was printed by any of this.
    for (int r = 0; r < 10; ++r) {
        QVERIFY(t.row(r).isEmpty());
    }
}

void Tst_ansiparser::oscTitle()
{
    Term t(3, 20);
    QSignalSpy title(&t.screen, &TerminalScreen::titleChanged);

    t.feed("\033]0;Hello\007");
    QCOMPARE(t.screen.title(), u"Hello"_s);
    QCOMPARE(title.count(), 1);
    QCOMPARE(title.last().at(0).toString(), u"Hello"_s);

    t.feed("\033]2;World\033\\x"); // ST terminator
    QCOMPARE(t.screen.title(), u"World"_s);
    QCOMPARE(t.row(0), u"x"_s);

    // Other OSC numbers are ignored, including hyperlinks and clipboard requests.
    t.feed("\033]52;c;AAAA\007\033]8;;http://example.com\007y\033]8;;\007\033]1;icon\007\033]10;?\007");
    QCOMPARE(t.screen.title(), u"World"_s);
    QCOMPARE(t.row(0), u"xy"_s);
    QCOMPARE(title.count(), 2);

    t.feed("\033]0;\xE6\x9D\xBF\xE5\xAD\x90\007"); // UTF-8 title
    QCOMPARE(t.screen.title(), u"板子"_s);
    t.feed("\033]0;\007"); // empty title
    QCOMPARE(t.screen.title(), u""_s);
    t.feed("\033]0;T\xC2\x9C"); // C1 ST
    QCOMPARE(t.screen.title(), u"T"_s);

    // An unterminated OSC keeps collecting across feeds.
    t.feed("\033]0;no-terminator");
    QCOMPARE(t.screen.title(), u"T"_s);
    t.feed(" continues\007");
    QCOMPARE(t.screen.title(), u"no-terminator continues"_s);

    // ESC not followed by '\' still ends the string and starts a new sequence.
    t.feed("\033]2;Abc\033[31mR");
    QCOMPARE(t.screen.title(), u"Abc"_s);
    QCOMPARE(t.cell(0, 2).ch, U'R');
    QCOMPARE(t.cell(0, 2).attr.fg, Color::indexed(1));

    // An OSC without ';' is ignored.
    t.feed("\033]0\007z");
    QCOMPARE(t.screen.title(), u"Abc"_s);
    QCOMPARE(t.cell(0, 3).ch, U'z');

    // A title with ';' in it keeps everything after the first separator.
    t.feed("\033]0;a;b;c\007");
    QCOMPARE(t.screen.title(), u"a;b;c"_s);

    // An over-long OSC is capped: the string collects at most 4096 code points including the
    // "0;" prefix (so 4094 of the title), the excess is dropped and the terminator is still
    // honoured, so the text after it is printed.
    t.feed("\033]0;" + QByteArray(5000, 'x') + "\007after");
    QCOMPARE(t.screen.title().size(), 4096 - 2);
    QVERIFY(t.screen.title() == QString(4096 - 2, u'x'));
    QVERIFY(t.row(0).endsWith(u"after"_s));
    QCOMPARE(t.cursorCol(), 9);
}

// ---- ESC sequences and resets -------------------------------------------------------------

void Tst_ansiparser::escSequences()
{
    Term t(5, 10, 10);
    t.feed("abc\033" "7\033[3;3H\033" "8"); // DECSC / DECRC
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 3);
    t.feed("\033D"); // IND
    QCOMPARE(t.cursorRow(), 1);
    QCOMPARE(t.cursorCol(), 3);
    t.feed("\033E"); // NEL
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 0);
    t.feed("\033M"); // RI
    QCOMPARE(t.cursorRow(), 1);
    t.feed("\033[Htop\033[H\033M"); // RI on the top line scrolls down
    QVERIFY(t.row(0).isEmpty());
    QCOMPARE(t.row(1), u"top"_s);
    QCOMPARE(t.cursorRow(), 0);
    t.feed("\033[4G\033H\033[G\tX"); // HTS
    QCOMPARE(t.cell(0, 3).ch, U'X');

    // Keypad selections are consumed; charset designations are recorded but GL stays G0 = ASCII
    // (graphics in G1 only show after SO).
    t.feed("\033[4;1H\033=\033>\033(B\033)0\033*A\033+B\033 F\033%Gok");
    QCOMPARE(t.row(3), u"ok"_s);

    // RIS resets the screen and the parser's own modes.
    t.feed("\033[?1h\033[?2004h\033[20h\033[31m\033[2;3r\033[?6h\033[?25l");
    QSignalSpy keyMode(&t.parser, &AnsiParser::cursorKeyModeChanged);
    QSignalSpy paste(&t.parser, &AnsiParser::bracketedPasteChanged);
    t.feed("\033c");
    for (int r = 0; r < 5; ++r) {
        QVERIFY(t.row(r).isEmpty());
    }
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 0);
    QVERIFY(t.screen.cursorVisible());
    QCOMPARE(t.screen.currentAttributes(), Attributes());
    QCOMPARE(t.screen.scrollTop(), 0);
    QCOMPARE(t.screen.scrollBottom(), 4);
    QVERIFY(!t.screen.originMode());
    QCOMPARE(t.screen.scrollbackSize(), 0);
    QCOMPARE(keyMode.count(), 1);
    QCOMPARE(keyMode.last().at(0).toBool(), false);
    QCOMPARE(paste.count(), 1);
    QCOMPARE(paste.last().at(0).toBool(), false);
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QVERIFY(!t.parser.bracketedPasteMode());
    // LNM was cleared by RIS: with implicitCr off a bare LF keeps the column.
    t.parser.setImplicitCr(false);
    t.feed("ab\nc");
    QCOMPARE(t.row(1), u"  c"_s);
}

void Tst_ansiparser::softReset()
{
    Term t(5, 10);
    t.feed("text");
    t.feed("\033[1;31m\033[2;4r\033[?6h\033[?7l\033[4h\033[?25l\033[?1h\033[?2004h\033[20h");
    t.feed("\033[2;3H"); // origin mode: row 2 of the region -> screen row 2
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 2);
    QSignalSpy keyMode(&t.parser, &AnsiParser::cursorKeyModeChanged);
    QSignalSpy paste(&t.parser, &AnsiParser::bracketedPasteChanged);

    t.feed("\033[!p"); // DECSTR
    QCOMPARE(t.screen.currentAttributes(), Attributes());
    QCOMPARE(t.screen.scrollTop(), 0);
    QCOMPARE(t.screen.scrollBottom(), 4);
    QVERIFY(!t.screen.originMode());
    QVERIFY(t.screen.autoWrap());
    QVERIFY(!t.screen.insertMode());
    QVERIFY(t.screen.cursorVisible());
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QVERIFY(!t.parser.bracketedPasteMode());
    QCOMPARE(keyMode.count(), 1);
    QCOMPARE(paste.count(), 1);
    QCOMPARE(t.cursorRow(), 2); // DECSTR does not move the cursor
    QCOMPARE(t.cursorCol(), 2);
    QCOMPARE(t.row(0), u"text"_s); // ... nor erase anything
    QCOMPARE(t.cell(0, 0).attr, Attributes());

    // LNM was reset too.
    t.parser.setImplicitCr(false);
    t.feed("\033[3;1Hab\nc");
    QCOMPARE(t.row(3), u"  c"_s);
}

void Tst_ansiparser::alignmentPattern()
{
    Term t(3, 5);
    t.feed("\033[2;3r\033[?6h\033[2;2Hxy");
    t.feed("\033#8"); // DECALN
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 5; ++c) {
            QCOMPARE(t.cell(r, c).ch, U'E');
        }
        QVERIFY(!t.screen.line(r).wrapped);
    }
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 0);
    QCOMPARE(t.screen.scrollTop(), 0);
    QCOMPARE(t.screen.scrollBottom(), 2);
    QVERIFY(!t.screen.originMode());
    // Other ESC # sequences (double-height / double-width) are ignored.
    t.feed("\033#3\033#4\033#5\033#6Q");
    QCOMPARE(t.cell(0, 0).ch, U'Q');
    QCOMPARE(t.cursorCol(), 1);
}

void Tst_ansiparser::decSpecialGraphics()
{
    // a. G0 = DEC Special Graphics draws ncurses box corners; ESC ( B returns to ASCII.
    Term t(3, 20);
    t.feed("\033(0lqqk\033(Bx");
    QCOMPARE(t.row(0), u"┌──┐x"_s);
    QCOMPARE(t.cursorCol(), 5);

    // b. Graphics designated into G1: SO switches to G1, SI back to G0.
    Term u(3, 20);
    u.feed("\033)0\016lqk\017lqk");
    QCOMPARE(u.row(0), u"┌─┐lqk"_s);

    // c. Only 0x5F..0x7E are remapped; space, digits, upper case and UTF-8 pass through.
    Term v(3, 20);
    v.feed("\033(0 A1z~");
    QCOMPARE(v.row(0), u" A1≥·"_s);
    v.feed("\xE4\xB8\xAD");
    QCOMPARE(v.row(0), u" A1≥·中"_s);
    QVERIFY(v.cell(0, 5).attr.has(WideLead));
    QCOMPARE(v.cursorCol(), 7);

    // d. RIS, DECSTR and AnsiParser::reset() all go back to ASCII in G0.
    Term r1(3, 20);
    r1.feed("\033(0\033cq");
    QCOMPARE(r1.row(0), u"q"_s);
    Term r2(3, 20);
    r2.feed("\033(0\033[!pq");
    QCOMPARE(r2.row(0), u"q"_s);
    Term r3(3, 20);
    r3.feed("\033(0");
    r3.parser.reset();
    r3.feed("q");
    QCOMPARE(r3.row(0), u"q"_s);
    Term r4(3, 20);
    r4.feed("\033)0\016\033cq"); // RIS also selects G0 again
    QCOMPARE(r4.row(0), u"q"_s);

    // e. DECSC / DECRC save and restore the designations and the active set.
    Term s1(3, 20);
    s1.feed("\033(0\033" "7\033(B\033" "8q");
    QCOMPARE(s1.row(0), u"─"_s);
    Term s2(3, 20);
    s2.feed("\033)0\016\033" "7\017\033" "8q");
    QCOMPARE(s2.row(0), u"─"_s);

    // f. A designation split across feed() calls.
    Term f(3, 20);
    f.feed("\033(");
    f.feed("0q");
    QCOMPARE(f.row(0), u"─"_s);

    // Other designations (UK, alternate ROM standard, DEC Supplemental) are ASCII; G2/G3 never
    // reach GL.
    Term o(3, 20);
    o.feed("\033(Aq\033(1q\033(%5q\033*0q\033+0q");
    QCOMPARE(o.row(0), u"qqqqq"_s);
    o.feed("\033(2q"); // alternate ROM special graphics
    QCOMPARE(o.row(0), u"qqqqq─"_s);
}

// ---- Robustness ---------------------------------------------------------------------------

void Tst_ansiparser::unknownSequencesIgnored_data()
{
    QTest::addColumn<QByteArray>("sequence");

    QTest::newRow("unknown DEC mode set") << "\033[?9999h"_ba;
    QTest::newRow("unknown DEC mode reset") << "\033[?9999l"_ba;
    QTest::newRow("unknown ANSI mode") << "\033[99h"_ba;
    QTest::newRow("XTMODKEYS") << "\033[>1;2m"_ba;
    QTest::newRow("XTQMODKEYS") << "\033[?4m"_ba;
    QTest::newRow("secondary DA") << "\033[>c"_ba;
    QTest::newRow("tertiary DA") << "\033[=c"_ba;
    QTest::newRow("DA reply echoed") << "\033[?1;2c"_ba;
    QTest::newRow("kitty keyboard") << "\033[>1u"_ba;
    QTest::newRow("kitty keyboard query") << "\033[?u"_ba;
    QTest::newRow("kitty keyboard pop") << "\033[<u"_ba;
    QTest::newRow("DECSCUSR") << "\033[2 q"_ba;
    QTest::newRow("DECSCUSR default") << "\033[ q"_ba;
    QTest::newRow("DECSCA") << "\033[1\"q"_ba;
    QTest::newRow("DECSCL") << "\033[62;1\"p"_ba;
    QTest::newRow("DECRQM") << "\033[?25$p"_ba;
    QTest::newRow("DECERA") << "\033[1;1;5;5$z"_ba;
    QTest::newRow("DECSACE") << "\033[2*x"_ba;
    QTest::newRow("DECIC") << "\033[3'}"_ba;
    QTest::newRow("XTWINOPS resize") << "\033[8;24;80t"_ba;
    QTest::newRow("XTWINOPS title stack") << "\033[22;0t"_ba;
    QTest::newRow("XTHIMOUSE") << "\033[2;2;2;2;2T"_ba;
    QTest::newRow("DECLL") << "\033[1q"_ba;
    QTest::newRow("DECREQTPARM") << "\033[1x"_ba;
    QTest::newRow("unknown final byte") << "\033[1y"_ba;
    QTest::newRow("unknown final byte 2") << "\033[3~"_ba;
    QTest::newRow("private then param (CSI ignore)") << "\033[1?2h"_ba;
    QTest::newRow("intermediate then param (CSI ignore)") << "\033[ 5q"_ba;
    QTest::newRow("too many parameters") << "\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;"
                                            "23;24;25;26;27;28;29;30;31;32;33;34;35;36;37;38;39;40t"_ba;
    QTest::newRow("too many sub-parameters") << "\033[1:2:3:4:5:6:7:8:9:10:11:12:13t"_ba;
    QTest::newRow("huge parameter") << "\033[99999999999999999999t"_ba;
    QTest::newRow("charset G0") << "\033(B"_ba;
    QTest::newRow("charset G0 graphics then back") << "\033(0\033(B"_ba;
    QTest::newRow("charset G1") << "\033)0"_ba;
    QTest::newRow("charset G2") << "\033*A"_ba;
    QTest::newRow("charset G3") << "\033+B"_ba;
    QTest::newRow("charset 96") << "\033-A"_ba;
    QTest::newRow("DECKPAM") << "\033="_ba;
    QTest::newRow("DECKPNM") << "\033>"_ba;
    QTest::newRow("S7C1T") << "\033 F"_ba;
    QTest::newRow("UTF-8 select") << "\033%G"_ba;
    QTest::newRow("DECDHL") << "\033#3"_ba;
    QTest::newRow("DECSWL") << "\033#5"_ba;
    QTest::newRow("unknown ESC final") << "\033k"_ba;
    QTest::newRow("SS2 / SS3") << "\033N\033O"_ba;
    QTest::newRow("LS2 / LS3") << "\033n\033o"_ba;
    QTest::newRow("APC") << "\033_apc payload 1;2;3\033\\"_ba;
    QTest::newRow("PM") << "\033^privacy message\033\\"_ba;
    QTest::newRow("SOS") << "\033Xstart of string\033\\"_ba;
    QTest::newRow("DCS sixel") << "\033Pq#0;2;0;0;0#0~~\033\\"_ba;
    QTest::newRow("DCS DECRPM") << "\033P1$r0;24r\033\\"_ba;
    QTest::newRow("DCS with garbage") << "\033P:\001\002garbage\033\\"_ba;
    QTest::newRow("DCS BEL terminated is not terminated") << "\033Pdata\007more\033\\"_ba;
    QTest::newRow("OSC hyperlink") << "\033]8;;http://example.com\033\\"_ba;
    QTest::newRow("OSC 1337") << "\033]1337;File=inline=1:AAAA\007"_ba;
    QTest::newRow("OSC colour query") << "\033]10;?\007"_ba;
    QTest::newRow("OSC 133 prompt marks") << "\033]133;A\007"_ba;
    QTest::newRow("lone ESC then text") << "\033"_ba; // the 'o' of "ok" becomes ESC o (LS3)
}

void Tst_ansiparser::unknownSequencesIgnored()
{
    QFETCH(QByteArray, sequence);
    Term t(3, 60);
    t.feed(sequence + "ok"_ba);
    if (sequence == "\033"_ba) {
        // ESC o is LS3 (ignored); only "k" remains printable.
        QCOMPARE(t.row(0), u"k"_s);
        return;
    }
    QCOMPARE(t.row(0), u"ok"_s);
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 2);
    QCOMPARE(t.screen.currentAttributes(), Attributes());
    QVERIFY(!t.screen.alternateScreenActive());
    QVERIFY(t.screen.cursorVisible());
    QVERIFY(t.screen.autoWrap());
    QVERIFY(!t.screen.originMode());
    QVERIFY(!t.screen.insertMode());
    QCOMPARE(t.screen.scrollTop(), 0);
    QCOMPARE(t.screen.scrollBottom(), 2);
    QVERIFY(t.screen.title().isEmpty());
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QVERIFY(!t.parser.bracketedPasteMode());
}

void Tst_ansiparser::garbageNeverDesyncs()
{
    Term t(5, 20, 50);
    QByteArray all;
    for (int b = 0; b < 256; ++b) {
        all.append(static_cast<char>(b));
    }
    t.feed(all);
    t.feed(all);
    QByteArray reversed = all;
    std::reverse(reversed.begin(), reversed.end());
    t.feed(reversed);

    // Pathological partial sequences.
    t.feed("\033[");
    t.feed(QByteArray(200, '9'));
    t.feed(QByteArray(100, ';'));
    t.feed(QByteArray(100, ':'));
    t.feed("\033]");
    t.feed(QByteArray(5000, 'x'));
    t.feed("\033P");
    t.feed(all);
    t.feed("\033[?");
    t.feed(all);
    t.feed("\033_");
    t.feed(reversed);
    // A stray DCS introducer followed by non-ASCII text: the string is abandoned and the text
    // printed, so the parser is back in Ground before the RIS below (the CRLF only pins the
    // column so the assertion does not depend on where the garbage left the cursor).
    t.feed("\r\n\033P");
    t.feed("\xC3\xA9");
    t.feed("z");
    QVERIFY(t.row(t.cursorRow()).endsWith(u"éz"_s));
    t.feed("\xF0\x9F"); // truncated 4-byte sequence

    // RIS is always a valid resynchronisation point.
    t.feed("\033c");
    t.feed("ok");
    QCOMPARE(t.row(0), u"ok"_s);
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 2);
    QCOMPARE(t.screen.currentAttributes(), Attributes());
    QVERIFY(!t.screen.alternateScreenActive());
    QVERIFY(!t.screen.originMode());
    QVERIFY(!t.screen.insertMode());
    QVERIFY(t.screen.autoWrap());
    QCOMPARE(t.screen.scrollbackSize(), 0);
    for (int r = 1; r < 5; ++r) {
        QVERIFY(t.row(r).isEmpty());
    }
}

void Tst_ansiparser::nonAsciiAbortsSequence_data()
{
    QTest::addColumn<QByteArray>("sequence");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<int>("cursorCol");

    // A printable code point >= U+00A0 inside an ESC / CSI sequence (or a DCS string) is line
    // noise: the sequence is abandoned and the character printed. OSC keeps collecting it.
    QTest::newRow("CSI param") << "\033[3\xE4\xB8\xADok"_ba << u"中ok"_s << 4;
    QTest::newRow("CSI entry") << "\033[\xC3\xA9ok"_ba << u"éok"_s << 3;
    QTest::newRow("CSI private") << "\033[?25\xC3\xA9ok"_ba << u"éok"_s << 3;
    QTest::newRow("CSI intermediate") << "\033[ \xC3\xA9ok"_ba << u"éok"_s << 3;
    QTest::newRow("CSI ignore") << "\033[1?2\xC3\xA9ok"_ba << u"éok"_s << 3;
    QTest::newRow("ESC") << "\033\xC3\xA9ok"_ba << u"éok"_s << 3;
    QTest::newRow("ESC intermediate") << "\033#\xC3\xA9ok"_ba << u"éok"_s << 3;
    QTest::newRow("NBSP") << "\033[3\xC2\xA0ok"_ba << QString(QChar(0xA0)) + u"ok"_s << 3;
    QTest::newRow("OSC not aborted") << "\033]0;\xE4\xB8\xAD\007ok"_ba << u"ok"_s << 2;
    QTest::newRow("DCS aborted") << "\033P\xE4\xB8\xAD\033\\ok"_ba << u"中ok"_s << 4;
}

void Tst_ansiparser::nonAsciiAbortsSequence()
{
    QFETCH(QByteArray, sequence);
    QFETCH(QString, expected);
    QFETCH(int, cursorCol);

    Term t(3, 20);
    t.feed(sequence);
    QCOMPARE(t.row(0), expected);
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), cursorCol);
    // Nothing of the abandoned sequence took effect.
    QCOMPARE(t.screen.currentAttributes(), Attributes());
    QVERIFY(t.screen.cursorVisible());
    QVERIFY(t.screen.autoWrap());
    QVERIFY(!t.screen.alternateScreenActive());
    if (sequence.startsWith("\033]"_ba)) {
        QCOMPARE(t.screen.title(), u"中"_s);
    } else {
        QVERIFY(t.screen.title().isEmpty());
    }
}

void Tst_ansiparser::controlStringGarbageRecovers()
{
    // A garbage ESC P / ESC X / ESC ^ / ESC _ from line noise must not swallow the console
    // until an ST arrives: a non-ASCII printable aborts the string and is printed.
    Term t(3, 40);
    t.feed("\033P\xC3\xA9ok"); // non-ASCII in DcsEntry
    QCOMPARE(t.row(0), u"éok"_s);
    t.feed("\r\n\033_apc\xFFmore"); // invalid UTF-8 -> U+FFFD aborts SosPmApcString
    QCOMPARE(t.row(1), QString(kReplacement) + u"more"_s);
    t.feed("\r\n\033Pq#0;2;0\xE2\x82\xACrest"); // non-ASCII in DcsPassthrough
    QCOMPARE(t.row(2), u"€rest"_s);

    // 8-bit encoding: a raw C1 introducer, then a Latin-1 letter aborts it.
    Term l(3, 40);
    QVERIFY(l.parser.setEncoding(u"ISO-8859-1"_s));
    l.feed("\x9Ejunk\xE9ok"); // 0x9E = PM
    QCOMPARE(l.row(0), u"éok"_s);

    // Length cap: ASCII-only garbage after a stray ESC X eventually prints again. The 4097th
    // code point ('\r') leaves the string; CR/LF and "visible" are then processed normally.
    Term c(3, 60);
    c.feed("\033X" + QByteArray(4096, 'x') + "\r\nvisible");
    QVERIFY(c.row(0).isEmpty());
    QCOMPARE(c.row(1), u"visible"_s);
    QCOMPARE(c.cursorRow(), 1);

    // Exactly at the cap the string is still open: the 4096th 'x' is consumed, ST closes it.
    Term d(3, 60);
    d.feed("\033X" + QByteArray(4096, 'x') + "\033\\ok");
    QCOMPARE(d.row(0), u"ok"_s);

    // C0 inside a well-formed string does not abort it (unchanged behaviour).
    Term k(3, 40);
    k.feed("\033Pdata\r\nmore\033\\ok");
    QCOMPARE(k.row(0), u"ok"_s);

    // OSC is deliberately excluded: titles may contain non-ASCII text.
    Term o(3, 40);
    o.feed("\033]0;\xC3\xA9t\xC3\xA9\007x");
    QCOMPARE(o.screen.title(), u"été"_s);
    QCOMPARE(o.row(0), u"x"_s);
}

void Tst_ansiparser::parserReset()
{
    Term t(3, 20);
    t.feed("\033[3");
    t.parser.reset();
    t.feed("1mx"); // the interrupted sequence is forgotten: printed literally
    QCOMPARE(t.row(0), u"1mx"_s);
    QCOMPARE(t.screen.currentAttributes(), Attributes());

    t.feed("\033[?1h\033[?2004h\033[20h");
    QSignalSpy keyMode(&t.parser, &AnsiParser::cursorKeyModeChanged);
    QSignalSpy paste(&t.parser, &AnsiParser::bracketedPasteChanged);
    t.parser.reset();
    QVERIFY(!t.parser.cursorKeyApplicationMode());
    QVERIFY(!t.parser.bracketedPasteMode());
    QCOMPARE(keyMode.count(), 1);
    QCOMPARE(paste.count(), 1);
    t.parser.setImplicitCr(false);
    t.feed("\r\nab\nc"); // LNM cleared by reset()
    QCOMPARE(t.row(2), u"  c"_s);

    // The decoder state is reset as well: a pending partial character is dropped.
    t.feed("\033[2J\033[H\xE4");
    t.parser.reset();
    t.feed("abc");
    QCOMPARE(t.row(0), u"abc"_s);

    // An unterminated OSC is dropped.
    t.feed("\033]0;partial");
    t.parser.reset();
    t.feed("def");
    QCOMPARE(t.row(0), u"abcdef"_s);
    QVERIFY(t.screen.title().isEmpty());

    // reset() does not touch the screen contents or options.
    QVERIFY(!t.parser.implicitCr());
    QCOMPARE(t.parser.encoding(), u"UTF-8"_s);
}

// ---- Realistic device output --------------------------------------------------------------

void Tst_ansiparser::wideCharacters()
{
    Term t(3, 10);
    t.feed("中文测试");
    QCOMPARE(t.row(0), u"中文测试"_s);
    QCOMPARE(t.cursorCol(), 8);
    QVERIFY(t.cell(0, 0).attr.has(WideLead));
    QVERIFY(t.cell(0, 1).isWideTrail());
    QVERIFY(t.cell(0, 6).attr.has(WideLead));
    QVERIFY(t.cell(0, 7).isWideTrail());
    t.feed("ok");
    QCOMPARE(t.row(0), u"中文测试ok"_s);
    QCOMPARE(t.cursorRow(), 0);
    QCOMPARE(t.cursorCol(), 9); // pending wrap on the last column

    // A wide character that does not fit in the last column wraps first.
    t.feed("\r\nabcdefghi中");
    QCOMPARE(t.row(1), u"abcdefghi"_s);
    QVERIFY(t.screen.line(1).wrapped);
    QCOMPARE(t.row(2), u"中"_s);
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 2);

    // Overwriting the trail half via cursor positioning blanks the lead.
    t.feed("\033[1;2HX");
    QCOMPARE(t.row(0), u" X文测试ok"_s);
    QVERIFY(!t.cell(0, 0).attr.has(WideLead));

    // Coloured Chinese text keeps its attributes on both halves.
    t.feed("\033[3;1H\033[32m汉\033[0m");
    QCOMPARE(t.cell(2, 0).attr.fg, Color::indexed(2));
    QCOMPARE(t.cell(2, 1).attr.fg, Color::indexed(2));
    QVERIFY(t.cell(2, 1).isWideTrail());
    QCOMPARE(t.screen.lineText(2), u"汉"_s);
}

void Tst_ansiparser::wrapAndScrollback()
{
    Term t(2, 5, 10);
    t.feed("abcdefghijkl");
    QCOMPARE(t.screen.scrollbackSize(), 1);
    QCOMPARE(t.screen.scrollbackLine(0).text(), u"abcde"_s);
    QVERIFY(t.screen.scrollbackLine(0).wrapped);
    QCOMPARE(t.row(0), u"fghij"_s);
    QVERIFY(t.screen.line(0).wrapped);
    QCOMPARE(t.row(1), u"kl"_s);
    QVERIFY(!t.screen.line(1).wrapped);
    QCOMPARE(t.screen.textRange(0, 0, 2, 5), u"abcdefghijkl"_s);

    t.feed("\r\n1\r\n2\r\n3");
    QCOMPARE(t.screen.scrollbackSize(), 4);
    QCOMPARE(t.screen.scrollbackLine(1).text(), u"fghij"_s);
    QVERIFY(t.screen.scrollbackLine(1).wrapped);
    QCOMPARE(t.screen.scrollbackLine(2).text(), u"kl"_s);
    QVERIFY(!t.screen.scrollbackLine(2).wrapped);
    QCOMPARE(t.screen.scrollbackLine(3).text(), u"1"_s);
    QCOMPARE(t.row(0), u"2"_s);
    QCOMPARE(t.row(1), u"3"_s);
    QCOMPARE(t.screen.textRange(0, 0, 5, 5), u"abcdefghijkl\n1\n2\n3"_s);
}

void Tst_ansiparser::bootLogSnippet()
{
    const QByteArray log =
        "\r\nU-Boot 2017.09 (Jan 01 2024 - 00:00:00 +0000)\r\n"
        "\r\n"
        "CPU: rockchip rv1106\r\n"
        "Hit any key to stop autoboot:  3 \b\b\b 2 \b\b\b 1 \b\b\b 0 \r\n"
        "[    0.000000] \033[32mBooting Linux\033[0m on physical CPU 0x0\r\n"
        "[    0.123456] \033[1;31mBUG\033[0m: bad thing\r\n"
        "\033[0;1;34m[  OK  ]\033[0m Started \033[1mNetwork\033[0m\r\n"
        "buildroot login: "_ba;

    const auto verify = [](const Term& t) {
        QCOMPARE(t.screen.scrollbackSize(), 1);
        QVERIFY(t.screen.scrollbackLine(0).text().isEmpty());
        QCOMPARE(t.row(0), u"U-Boot 2017.09 (Jan 01 2024 - 00:00:00 +0000)"_s);
        QVERIFY(t.row(1).isEmpty());
        QCOMPARE(t.row(2), u"CPU: rockchip rv1106"_s);
        QCOMPARE(t.row(3), u"Hit any key to stop autoboot:  0"_s);
        QCOMPARE(t.row(4), u"[    0.000000] Booting Linux on physical CPU 0x0"_s);
        QCOMPARE(t.row(5), u"[    0.123456] BUG: bad thing"_s);
        QCOMPARE(t.row(6), u"[  OK  ] Started Network"_s);
        QCOMPARE(t.row(7), u"buildroot login:"_s);
        QCOMPARE(t.cursorRow(), 7);
        QCOMPARE(t.cursorCol(), 17);

        // "Booting Linux" is green, the text after the reset is default.
        QCOMPARE(t.cell(4, 15).ch, U'B');
        QCOMPARE(t.cell(4, 15).attr.fg, Color::indexed(2));
        QVERIFY(!t.cell(4, 15).attr.has(Bold));
        QCOMPARE(t.cell(4, 27).attr.fg, Color::indexed(2));
        QVERIFY(t.cell(4, 29).attr.fg.isDefault());
        QCOMPARE(t.cell(4, 0).attr, Attributes());
        // "BUG" is bold red; the colon after it is plain.
        QCOMPARE(t.cell(5, 15).ch, U'B');
        QCOMPARE(t.cell(5, 15).attr.fg, Color::indexed(1));
        QVERIFY(t.cell(5, 15).attr.has(Bold));
        QCOMPARE(t.cell(5, 18).ch, U':');
        QCOMPARE(t.cell(5, 18).attr, Attributes());
        // "[  OK  ]" is bold blue, "Network" bold default.
        QCOMPARE(t.cell(6, 0).attr.fg, Color::indexed(4));
        QVERIFY(t.cell(6, 0).attr.has(Bold));
        QCOMPARE(t.cell(6, 8).attr, Attributes());
        QCOMPARE(t.cell(6, 17).ch, U'N');
        QVERIFY(t.cell(6, 17).attr.has(Bold));
        QVERIFY(t.cell(6, 17).attr.fg.isDefault());
        QCOMPARE(t.screen.currentAttributes(), Attributes());
    };

    // Whole log in one feed.
    Term whole(8, 60, 100);
    whole.feed(log);
    verify(whole);
    if (QTest::currentTestFailed()) {
        return;
    }

    // The same log in 3-byte chunks must produce the identical screen.
    Term chunked(8, 60, 100);
    for (qsizetype i = 0; i < log.size(); i += 3) {
        chunked.feed(log.mid(i, 3));
    }
    verify(chunked);
    if (QTest::currentTestFailed()) {
        return;
    }
    for (int r = 0; r < 8; ++r) {
        QVERIFY(chunked.screen.line(r).cells == whole.screen.line(r).cells);
        QCOMPARE(chunked.screen.line(r).wrapped, whole.screen.line(r).wrapped);
    }

    // And one byte at a time.
    Term bytewise(8, 60, 100);
    for (const char ch : log) {
        bytewise.feed(QByteArray(1, ch));
    }
    verify(bytewise);
    for (int r = 0; r < 8; ++r) {
        QVERIFY(bytewise.screen.line(r).cells == whole.screen.line(r).cells);
    }
}

void Tst_ansiparser::busyboxLineEditing()
{
    Term t(3, 20);
    t.feed("# abc");
    t.feed("\b \b"); // busybox erases the last character with BS SPACE BS
    QCOMPARE(t.row(0), u"# ab"_s);
    QCOMPARE(t.cursorCol(), 4);
    t.feed("\b \b\b \b");
    QCOMPARE(t.row(0), u"#"_s);
    QCOMPARE(t.row(0).size(), 1);
    QCOMPARE(t.cursorCol(), 2);

    t.feed("abcdef");
    QCOMPARE(t.row(0), u"# abcdef"_s);
    t.feed("\033[3D"); // cursor left 3
    QCOMPARE(t.cursorCol(), 5);
    t.feed("\033[K"); // erase to end of line
    QCOMPARE(t.row(0), u"# abc"_s);
    QCOMPARE(t.cursorCol(), 5);
    t.feed("\b\033[K"); // BS then EL: deletes one character
    QCOMPARE(t.row(0), u"# ab"_s);
    QCOMPARE(t.cursorCol(), 4);

    // Inserting in the middle of the line (ash redraws the tail and moves back).
    t.feed("X ab\033[2D");
    QCOMPARE(t.row(0), u"# abX ab"_s);
    QCOMPARE(t.cursorCol(), 6);

    // Ctrl+U style clear: CR, EL 2, redraw the prompt.
    t.feed("\r\033[2K# ");
    QCOMPARE(t.row(0), u"#"_s);
    QCOMPARE(t.cursorCol(), 2);

    // History recall: erase the whole input with EL 0 from the prompt then print.
    t.feed("ls -l\r\033[2C\033[Kcat /proc/cpuinfo");
    QCOMPARE(t.row(0), u"# cat /proc/cpuinfo"_s);
    QCOMPARE(t.cursorCol(), 19);
}

void Tst_ansiparser::progressLines()
{
    Term t(3, 30);
    t.feed("Loading 10%\rLoading 50%\rLoading 100%");
    QCOMPARE(t.row(0), u"Loading 100%"_s);
    QCOMPARE(t.cursorRow(), 0);
    t.feed("\r\033[Kdone");
    QCOMPARE(t.row(0), u"done"_s);
    t.feed("\r\n|\b/\b-\b*");
    QCOMPARE(t.row(1), u"*"_s);
    QCOMPARE(t.cursorCol(), 1);
    // A progress line that overruns the width wraps; CR then rewrites only the wrapped tail.
    t.feed("\r\n" + QByteArray(35, '#') + "\rshort");
    QCOMPARE(t.screen.scrollbackSize(), 1); // "done" scrolled off when the '#' line wrapped
    QCOMPARE(t.screen.scrollbackLine(0).text(), u"done"_s);
    QCOMPARE(t.row(0), u"*"_s);
    QCOMPARE(t.row(1), QString(30, u'#'));
    QVERIFY(t.screen.line(1).wrapped);
    QCOMPARE(t.row(2), u"short"_s);
    QCOMPARE(t.cursorRow(), 2);
    QCOMPARE(t.cursorCol(), 5);
}

void Tst_ansiparser::contentChangedBatching()
{
    Term t(5, 40);
    QSignalSpy content(&t.screen, &TerminalScreen::contentChanged);
    t.feed("hello world");
    QCOMPARE(content.count(), 1); // one putText() for the whole run
    t.feed("\033[1;31m");
    QCOMPARE(content.count(), 1); // attribute changes alone do not repaint
    t.feed("\033[2J");
    QCOMPARE(content.count(), 2);
    t.feed("abc\033[32mdef");
    QCOMPARE(content.count(), 4); // two runs
    t.feed(QByteArray(1000, 'x'));
    QCOMPARE(content.count(), 5);
}

void Tst_ansiparser::highVolumeOutput()
{
    QByteArray data;
    for (int i = 0; i < 1000; ++i) {
        data += "line " + QByteArray::number(i) + "\r\n";
    }

    Term t(5, 20, 50);
    t.feed(data);
    QCOMPARE(t.screen.scrollbackSize(), 50);
    QCOMPARE(t.screen.scrollbackLine(0).text(), u"line 946"_s);
    QCOMPARE(t.screen.scrollbackLine(49).text(), u"line 995"_s);
    QCOMPARE(t.row(0), u"line 996"_s);
    QCOMPARE(t.row(3), u"line 999"_s);
    QVERIFY(t.row(4).isEmpty());
    QCOMPARE(t.cursorRow(), 4);
    QCOMPARE(t.cursorCol(), 0);

    // Same data one byte at a time gives the same result.
    Term b(5, 20, 50);
    for (const char ch : data) {
        b.feed(QByteArray(1, ch));
    }
    QCOMPARE(b.screen.scrollbackSize(), 50);
    for (int i = 0; i < 50; ++i) {
        QCOMPARE(b.screen.scrollbackLine(i).text(), t.screen.scrollbackLine(i).text());
    }
    for (int r = 0; r < 5; ++r) {
        QCOMPARE(b.row(r), t.row(r));
    }
    QVERIFY(b.screen.cursor() == t.screen.cursor());
}

QTEST_GUILESS_MAIN(Tst_ansiparser)
#include "tst_ansiparser.moc"
