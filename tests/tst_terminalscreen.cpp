// Unit tests for TerminalScreen (src/terminal/TerminalScreen.h).
#include <QtTest>
#include <QSignalSpy>

#include "terminal/TerminalScreen.h"
#include "terminal/TerminalTypes.h"

using namespace Terminal;

namespace {

QString rowText(const TerminalScreen& s, int row)
{
    return s.line(row).text();
}

/// Writes one string per row starting at column 0, leaving the cursor after the last one.
void fillRows(TerminalScreen& s, const QStringList& rows)
{
    for (int i = 0; i < rows.size(); ++i) {
        s.moveCursorTo(i, 0);
        s.putText(rows.at(i));
    }
}

Attributes bgAttr(int index)
{
    Attributes a;
    a.bg = Color::indexed(index);
    return a;
}

} // namespace

class Tst_terminalscreen : public QObject
{
    Q_OBJECT
private slots:
    void defaults();
    void putCharAdvancesAndEmitsOnce();
    void putTextEmitsOnce();
    void putCharIgnoresControls();
    void pendingWrap();
    void pendingWrapClearedByCursorOps();
    void autoWrapOff();
    void lineFeedScrollsIntoScrollbackWithCap();
    void wrappedFlagPreservedInScrollback();
    void lineFeedInsideRegionDoesNotPush();
    void lineFeedBelowRegion();
    void backspaceNeverWrapsOrErases();
    void tabs();
    void cursorClamping();
    void moveCursorByStaysInRegion();
    void originMode();
    void eraseInDisplay();
    void eraseInDisplayScrollback();
    void eraseInLine();
    void eraseUsesBackgroundColour();
    void insertDeleteLines();
    void insertDeleteLinesOutsideRegionIgnored();
    void insertDeleteEraseChars();
    void scrollUpDown();
    void scrollRegionBehaviour();
    void reverseIndexAndNextLine();
    void cursorLineOps();
    void repeatLastChar();
    void insertMode();
    void saveRestoreCursor();
    void alternateScreenRoundTrip();
    void alternateScreenWithoutCursorSave();
    void resizeShrinkMovesLinesToScrollback();
    void resizeGrowPullsLinesBack();
    void resizeShrinkDropsBlankBottomLines();
    void resizeInAlternateScreenPreservesPrimary();
    void resizeColumns();
    void wideCharPlacement();
    void wideCharOverwrite();
    void wideCharAtLastColumn();
    void wideCharInsertMode();
    void textRangeAndLineText();
    void textRangeJoinsWrappedLines();
    void textRangeNormalisesAndClamps();
    void wordBoundsAt();
    void dirtyTracking();
    void reset();
    void clearScreenAndScrollback();
    void pushScreenToScrollback();
    void setScrollbackMaxTrims();
    void scrollbackDroppedCounts();
    void titleAndBell();
    void cursorVisibility();
    void setScrollRegionValidation();
};

void Tst_terminalscreen::defaults()
{
    TerminalScreen s;
    QCOMPARE(s.rows(), 24);
    QCOMPARE(s.cols(), 80);
    QCOMPARE(s.scrollbackMax(), 10000);
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    QVERIFY(s.cursorVisible());
    QCOMPARE(s.scrollTop(), 0);
    QCOMPARE(s.scrollBottom(), 23);
    QVERIFY(s.autoWrap());
    QVERIFY(!s.originMode());
    QVERIFY(!s.insertMode());
    QVERIFY(!s.alternateScreenActive());
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(s.totalLines(), 24);
    QVERIFY(s.title().isEmpty());
    QVERIFY(s.isDirty());
    QVERIFY(s.allDirty());
    QCOMPARE(s.currentAttributes(), Attributes());
    QCOMPARE(s.line(0).length(), 80);
    QVERIFY(rowText(s, 0).isEmpty());

    TerminalScreen tiny(0, 0, -1);
    QCOMPARE(tiny.rows(), 1);
    QCOMPARE(tiny.cols(), 1);
    QCOMPARE(tiny.scrollbackMax(), 0);
}

void Tst_terminalscreen::putCharAdvancesAndEmitsOnce()
{
    TerminalScreen s(5, 10);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    QSignalSpy moved(&s, &TerminalScreen::cursorMoved);
    s.putChar(U'a');
    s.putChar(U'b');
    QCOMPARE(rowText(s, 0), QStringLiteral("ab"));
    QCOMPARE(s.cursor().col, 2);
    QCOMPARE(content.count(), 2);
    QCOMPARE(moved.count(), 2);
    QCOMPARE(moved.last().at(0).toInt(), 0);
    QCOMPARE(moved.last().at(1).toInt(), 2);
}

void Tst_terminalscreen::putTextEmitsOnce()
{
    TerminalScreen s(5, 10);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.putText(QStringLiteral("hello"));
    QCOMPARE(rowText(s, 0), QStringLiteral("hello"));
    QCOMPARE(s.cursor().col, 5);
    QCOMPARE(content.count(), 1);

    // Surrogate pairs are combined into one code point.
    s.putText(QString::fromUcs4(U"\U0001F600"));
    QCOMPARE(s.line(0).cells[5].ch, char32_t(0x1F600));
    QVERIFY(s.line(0).cells[5].attr.has(WideLead));
    QCOMPARE(content.count(), 2);

    s.putText(QString());
    QCOMPARE(content.count(), 2);
}

void Tst_terminalscreen::putCharIgnoresControls()
{
    TerminalScreen s(5, 10);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.putChar(U'\n');
    s.putChar(0x7F);
    s.putChar(0x1B);
    s.putChar(0x9B);
    s.putChar(0x0301); // combining acute: zero width, ignored
    s.putChar(0xFEFF); // BOM / ZWNBSP: zero width, ignored
    QCOMPARE(content.count(), 0);
    QCOMPARE(s.cursor().col, 0);
    QVERIFY(rowText(s, 0).isEmpty());
}

void Tst_terminalscreen::pendingWrap()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abcde"));
    // Cursor stays on the last column, nothing wrapped yet.
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 4);
    QVERIFY(!s.line(0).wrapped);

    s.putChar(U'f');
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 1);
    QVERIFY(s.line(0).wrapped);
    QCOMPARE(rowText(s, 0), QStringLiteral("abcde"));
    QCOMPARE(rowText(s, 1), QStringLiteral("f"));
}

void Tst_terminalscreen::pendingWrapClearedByCursorOps()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abcde"));
    s.carriageReturn();
    s.putChar(U'X');
    QCOMPARE(rowText(s, 0), QStringLiteral("Xbcde"));
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 1);
    QVERIFY(!s.line(0).wrapped);

    s.moveCursorTo(0, 4);
    s.putChar(U'Y'); // sets pending wrap again
    s.moveCursorTo(0, 4);
    s.putChar(U'Z'); // explicit move cleared it: overwrite the last column
    QCOMPARE(rowText(s, 0), QStringLiteral("XbcdZ"));
    QCOMPARE(s.cursor().row, 0);

    s.putText(QStringLiteral("Q")); // now the pending wrap fires
    QCOMPARE(rowText(s, 1), QStringLiteral("Q"));

    // LF while pending: only one line feed, no double wrap.
    s.moveCursorTo(1, 0);
    s.putText(QStringLiteral("abcde"));
    s.lineFeed();
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(s.cursor().col, 4);
    s.carriageReturn();
    s.putChar(U'k');
    QCOMPARE(rowText(s, 2), QStringLiteral("k"));
    QVERIFY(!s.line(1).wrapped);
}

void Tst_terminalscreen::autoWrapOff()
{
    TerminalScreen s(3, 5);
    s.setAutoWrap(false);
    QVERIFY(!s.autoWrap());
    s.putText(QStringLiteral("abcdefg"));
    QCOMPARE(rowText(s, 0), QStringLiteral("abcdg"));
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 4);
    QVERIFY(rowText(s, 1).isEmpty());
    QVERIFY(!s.line(0).wrapped);
}

void Tst_terminalscreen::lineFeedScrollsIntoScrollbackWithCap()
{
    TerminalScreen s(3, 5, 2);
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")});
    QCOMPARE(s.cursor().row, 2);
    s.lineFeed();
    QCOMPARE(s.scrollbackSize(), 1);
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("1"));
    QCOMPARE(rowText(s, 0), QStringLiteral("2"));
    QCOMPARE(rowText(s, 1), QStringLiteral("3"));
    QVERIFY(rowText(s, 2).isEmpty());
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(sbChanged.count(), 1);
    QCOMPARE(sbChanged.last().at(0).toInt(), 1);

    s.carriageReturn();
    s.putText(QStringLiteral("4"));
    s.lineFeed();
    s.carriageReturn();
    s.putText(QStringLiteral("5"));
    s.lineFeed();
    QCOMPARE(s.scrollbackSize(), 2); // capped, oldest dropped
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("2"));
    QCOMPARE(s.scrollbackLine(1).text(), QStringLiteral("3"));
    QCOMPARE(s.totalLines(), 5);
    QCOMPARE(s.absoluteLine(0).text(), QStringLiteral("2"));
    QCOMPARE(s.absoluteLine(2).text(), QStringLiteral("4"));
    QCOMPARE(s.lineText(3), QStringLiteral("5"));

    // No scrollback at all when the max is zero.
    TerminalScreen none(2, 5, 0);
    none.putText(QStringLiteral("a"));
    none.lineFeed();
    none.lineFeed();
    QCOMPARE(none.scrollbackSize(), 0);
}

void Tst_terminalscreen::wrappedFlagPreservedInScrollback()
{
    TerminalScreen s(2, 5, 10);
    s.putText(QStringLiteral("abcdefghijkl"));
    QCOMPARE(s.scrollbackSize(), 1);
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("abcde"));
    QVERIFY(s.scrollbackLine(0).wrapped);
    QCOMPARE(rowText(s, 0), QStringLiteral("fghij"));
    QVERIFY(s.line(0).wrapped);
    QCOMPARE(rowText(s, 1), QStringLiteral("kl"));
    QVERIFY(!s.line(1).wrapped);
    QCOMPARE(s.textRange(0, 0, 2, 5), QStringLiteral("abcdefghijkl"));
}

void Tst_terminalscreen::lineFeedInsideRegionDoesNotPush()
{
    TerminalScreen s(5, 5, 10);
    fillRows(s, {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
                 QStringLiteral("4")});
    s.setScrollRegion(1, 3);
    s.moveCursorTo(3, 0);
    s.lineFeed();
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(rowText(s, 0), QStringLiteral("0"));
    QCOMPARE(rowText(s, 1), QStringLiteral("2"));
    QCOMPARE(rowText(s, 2), QStringLiteral("3"));
    QVERIFY(rowText(s, 3).isEmpty());
    QCOMPARE(rowText(s, 4), QStringLiteral("4"));
    QCOMPARE(s.cursor().row, 3);
}

void Tst_terminalscreen::lineFeedBelowRegion()
{
    TerminalScreen s(5, 5, 10);
    fillRows(s, {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
                 QStringLiteral("4")});
    s.setScrollRegion(0, 2);
    s.moveCursorTo(3, 0);
    s.lineFeed();
    QCOMPARE(s.cursor().row, 4);
    s.lineFeed(); // at the last row, outside the region: nothing scrolls
    QCOMPARE(s.cursor().row, 4);
    QCOMPARE(rowText(s, 0), QStringLiteral("0"));
    QCOMPARE(rowText(s, 4), QStringLiteral("4"));
    QCOMPARE(s.scrollbackSize(), 0);
}

void Tst_terminalscreen::backspaceNeverWrapsOrErases()
{
    TerminalScreen s(3, 5);
    s.backspace();
    QCOMPARE(s.cursor().col, 0);
    QCOMPARE(s.cursor().row, 0);
    s.putText(QStringLiteral("abc"));
    s.backspace();
    QCOMPARE(s.cursor().col, 2);
    QCOMPARE(rowText(s, 0), QStringLiteral("abc"));
    s.moveCursorTo(1, 0);
    s.backspace();
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 0);

    // busybox line editing: "abc" BS ' ' BS
    s.moveCursorTo(2, 0);
    s.putText(QStringLiteral("abc"));
    s.backspace();
    s.putChar(U' ');
    s.backspace();
    QCOMPARE(rowText(s, 2), QStringLiteral("ab"));
    QCOMPARE(s.cursor().col, 2);
}

void Tst_terminalscreen::tabs()
{
    TerminalScreen s(3, 80);
    s.tab();
    QCOMPARE(s.cursor().col, 8);
    s.tab();
    QCOMPARE(s.cursor().col, 16);
    s.moveCursorTo(0, 75);
    s.tab();
    QCOMPARE(s.cursor().col, 79);
    s.tab();
    QCOMPARE(s.cursor().col, 79);

    s.clearAllTabStops();
    s.moveCursorTo(0, 3);
    s.setTabStop();
    s.moveCursorTo(0, 0);
    s.tab();
    QCOMPARE(s.cursor().col, 3);
    s.tab();
    QCOMPARE(s.cursor().col, 79);

    s.resetTabStops();
    s.moveCursorTo(0, 8);
    s.clearTabStop();
    s.moveCursorTo(0, 0);
    s.tab();
    QCOMPARE(s.cursor().col, 16);

    // Tab does not modify cells.
    s.moveCursorTo(1, 0);
    s.putText(QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
    s.moveCursorTo(1, 0);
    s.tab();
    QCOMPARE(rowText(s, 1), QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
}

void Tst_terminalscreen::cursorClamping()
{
    TerminalScreen s(24, 80);
    s.moveCursorTo(100, 100);
    QCOMPARE(s.cursor().row, 23);
    QCOMPARE(s.cursor().col, 79);
    s.moveCursorTo(-5, -5);
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    s.moveCursorBy(-10, -10);
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    s.moveCursorBy(3, 4);
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 4);
    s.moveCursorBy(0, 1000);
    QCOMPARE(s.cursor().col, 79);
    s.moveCursorBy(1000, 0);
    QCOMPARE(s.cursor().row, 23);
    s.moveCursorBy(-2, -3);
    QCOMPARE(s.cursor().row, 21);
    QCOMPARE(s.cursor().col, 76);
    s.setCursorRow(-1);
    QCOMPARE(s.cursor().row, 0);
    s.setCursorRow(99);
    QCOMPARE(s.cursor().row, 23);
    s.setCursorColumn(99);
    QCOMPARE(s.cursor().col, 79);
    s.setCursorColumn(5);
    QCOMPARE(s.cursor().col, 5);
}

void Tst_terminalscreen::moveCursorByStaysInRegion()
{
    TerminalScreen s(24, 80);
    s.setScrollRegion(5, 10);
    QCOMPARE(s.cursor().row, 0); // DECSTBM homes the cursor
    s.moveCursorTo(7, 0);
    s.moveCursorBy(-10, 0);
    QCOMPARE(s.cursor().row, 5);
    s.moveCursorBy(10, 0);
    QCOMPARE(s.cursor().row, 10);
    // Outside the region the whole screen is the limit.
    s.moveCursorTo(2, 0);
    s.moveCursorBy(-5, 0);
    QCOMPARE(s.cursor().row, 0);
    s.moveCursorTo(15, 0);
    s.moveCursorBy(50, 0);
    QCOMPARE(s.cursor().row, 23);
    // Moving toward the region from outside stops at the margin being approached (xterm).
    s.moveCursorTo(15, 0);
    s.moveCursorBy(-20, 0);
    QCOMPARE(s.cursor().row, 5); // up from below stops at the top margin
    s.moveCursorTo(2, 0);
    s.moveCursorBy(20, 0);
    QCOMPARE(s.cursor().row, 10); // down from above stops at the bottom margin
    s.moveCursorTo(2, 0);
    s.moveCursorBy(2, 0);
    QCOMPARE(s.cursor().row, 4); // a short move that does not reach the margin is unaffected
    s.moveCursorTo(15, 0);
    s.moveCursorBy(-3, 0);
    QCOMPARE(s.cursor().row, 12);
}

void Tst_terminalscreen::originMode()
{
    TerminalScreen s(24, 80);
    s.setScrollRegion(5, 10);
    s.setOriginMode(true);
    QVERIFY(s.originMode());
    QCOMPARE(s.cursor().row, 5);
    QCOMPARE(s.cursor().col, 0);
    s.moveCursorTo(0, 0);
    QCOMPARE(s.cursor().row, 5);
    s.moveCursorTo(2, 3);
    QCOMPARE(s.cursor().row, 7);
    QCOMPARE(s.cursor().col, 3);
    s.moveCursorTo(100, 0);
    QCOMPARE(s.cursor().row, 10);
    s.setCursorRow(1);
    QCOMPARE(s.cursor().row, 6);
    s.setCursorRow(50);
    QCOMPARE(s.cursor().row, 10);
    // setScrollRegion in origin mode homes to the region top.
    s.setScrollRegion(2, 20);
    QCOMPARE(s.cursor().row, 2);
    s.setOriginMode(false);
    QCOMPARE(s.cursor().row, 0);
    s.moveCursorTo(0, 0);
    QCOMPARE(s.cursor().row, 0);
}

void Tst_terminalscreen::eraseInDisplay()
{
    TerminalScreen s(5, 5, 10);
    const QStringList rows{QStringLiteral("xxxxx"), QStringLiteral("xxxxx"), QStringLiteral("xxxxx"),
                           QStringLiteral("xxxxx"), QStringLiteral("xxxxx")};
    fillRows(s, rows);
    s.moveCursorTo(2, 2);
    s.eraseInDisplay(0);
    QCOMPARE(rowText(s, 0), QStringLiteral("xxxxx"));
    QCOMPARE(rowText(s, 1), QStringLiteral("xxxxx"));
    QCOMPARE(rowText(s, 2), QStringLiteral("xx"));
    QVERIFY(rowText(s, 3).isEmpty());
    QVERIFY(rowText(s, 4).isEmpty());
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(s.cursor().col, 2);

    fillRows(s, rows);
    s.moveCursorTo(2, 2);
    s.eraseInDisplay(1);
    QVERIFY(rowText(s, 0).isEmpty());
    QVERIFY(rowText(s, 1).isEmpty());
    QCOMPARE(rowText(s, 2), QStringLiteral("   xx"));
    QCOMPARE(rowText(s, 3), QStringLiteral("xxxxx"));

    fillRows(s, rows);
    s.moveCursorTo(2, 2);
    s.eraseInDisplay(2);
    for (int r = 0; r < 5; ++r) {
        QVERIFY(rowText(s, r).isEmpty());
    }
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(s.cursor().col, 2);

    // An unknown mode is ignored.
    fillRows(s, rows);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.eraseInDisplay(7);
    QCOMPARE(content.count(), 0);
    QCOMPARE(rowText(s, 0), QStringLiteral("xxxxx"));
}

void Tst_terminalscreen::eraseInDisplayScrollback()
{
    TerminalScreen s(2, 5, 10);
    s.putText(QStringLiteral("a"));
    s.lineFeed();
    s.lineFeed();
    s.carriageReturn();
    s.putText(QStringLiteral("b"));
    QCOMPARE(s.scrollbackSize(), 1);
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    s.eraseInDisplay(3);
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(sbChanged.count(), 1);
    QCOMPARE(sbChanged.last().at(0).toInt(), 0);
    QCOMPARE(rowText(s, 1), QStringLiteral("b")); // screen untouched
}

void Tst_terminalscreen::eraseInLine()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 2);
    s.eraseInLine(0);
    QCOMPARE(rowText(s, 0), QStringLiteral("ab"));
    QCOMPARE(s.cursor().col, 2);

    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 2);
    s.eraseInLine(1);
    QCOMPARE(rowText(s, 0), QStringLiteral("   de"));

    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 2);
    s.eraseInLine(2);
    QVERIFY(rowText(s, 0).isEmpty());
    QCOMPARE(s.line(0).length(), 5);

    // EL 0 on a wrapped line clears the wrapped flag; other rows are untouched.
    s.moveCursorTo(1, 0);
    s.putText(QStringLiteral("abcdef"));
    QVERIFY(s.line(1).wrapped);
    s.moveCursorTo(1, 3);
    s.eraseInLine(0);
    QVERIFY(!s.line(1).wrapped);
    QCOMPARE(rowText(s, 2), QStringLiteral("f"));
}

void Tst_terminalscreen::eraseUsesBackgroundColour()
{
    TerminalScreen s(3, 5);
    Attributes a = bgAttr(4);
    a.set(Bold, true);
    a.fg = Color::indexed(1);
    s.setCurrentAttributes(a);
    s.putText(QStringLiteral("ab"));
    s.eraseInLine(2);
    for (int c = 0; c < 5; ++c) {
        const Cell& cell = s.line(0).cells[c];
        QCOMPARE(cell.ch, U' ');
        QCOMPARE(cell.attr.bg, Color::indexed(4));
        QVERIFY(cell.attr.fg.isDefault());
        QCOMPARE(cell.attr.flags, quint16(NoAttr));
    }
    s.eraseInDisplay(2);
    QCOMPARE(s.line(2).cells[4].attr.bg, Color::indexed(4));
    // Scrolled-in lines also carry the background (bce).
    s.moveCursorTo(2, 0);
    s.lineFeed();
    QCOMPARE(s.line(2).cells[0].attr.bg, Color::indexed(4));
    s.resetAttributes();
    QCOMPARE(s.currentAttributes(), Attributes());
    s.eraseInLine(2);
    QVERIFY(s.line(2).cells[0].attr.bg.isDefault());
}

void Tst_terminalscreen::insertDeleteLines()
{
    TerminalScreen s(5, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
                 QStringLiteral("5")});
    s.moveCursorTo(1, 3);
    s.insertLines(2);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QVERIFY(rowText(s, 1).isEmpty());
    QVERIFY(rowText(s, 2).isEmpty());
    QCOMPARE(rowText(s, 3), QStringLiteral("2"));
    QCOMPARE(rowText(s, 4), QStringLiteral("3"));
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 0);
    QCOMPARE(s.scrollbackSize(), 0);

    s.moveCursorTo(1, 2);
    s.deleteLines(1);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QVERIFY(rowText(s, 1).isEmpty());
    QCOMPARE(rowText(s, 2), QStringLiteral("2"));
    QCOMPARE(rowText(s, 3), QStringLiteral("3"));
    QVERIFY(rowText(s, 4).isEmpty());
    QCOMPARE(s.cursor().col, 0);

    // Huge counts are clamped to the region.
    s.moveCursorTo(3, 0);
    s.deleteLines(100);
    QCOMPARE(rowText(s, 2), QStringLiteral("2"));
    QVERIFY(rowText(s, 3).isEmpty());
    QVERIFY(rowText(s, 4).isEmpty());

    // Inside a region the lines that fall off stay within the region.
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
                 QStringLiteral("5")});
    s.setScrollRegion(1, 3);
    s.moveCursorTo(1, 0);
    s.insertLines(1);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QVERIFY(rowText(s, 1).isEmpty());
    QCOMPARE(rowText(s, 2), QStringLiteral("2"));
    QCOMPARE(rowText(s, 3), QStringLiteral("3"));
    QCOMPARE(rowText(s, 4), QStringLiteral("5"));
}

void Tst_terminalscreen::insertDeleteLinesOutsideRegionIgnored()
{
    TerminalScreen s(5, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
                 QStringLiteral("5")});
    s.setScrollRegion(2, 4);
    s.moveCursorTo(0, 1);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.insertLines(1);
    s.deleteLines(1);
    QCOMPARE(content.count(), 0);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QCOMPARE(rowText(s, 1), QStringLiteral("2"));
    QCOMPARE(s.cursor().col, 1);
}

void Tst_terminalscreen::insertDeleteEraseChars()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 1);
    s.insertChars(2);
    QCOMPARE(rowText(s, 0), QStringLiteral("a  bc"));
    QCOMPARE(s.cursor().col, 1);
    s.deleteChars(1);
    QCOMPARE(rowText(s, 0), QStringLiteral("a bc"));
    QCOMPARE(s.line(0).length(), 5);
    s.eraseChars(2);
    QCOMPARE(rowText(s, 0), QStringLiteral("a  c"));
    QCOMPARE(s.cursor().col, 1);

    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 3);
    s.insertChars(100);
    QCOMPARE(rowText(s, 0), QStringLiteral("abc"));
    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 1);
    s.deleteChars(100);
    QCOMPARE(rowText(s, 0), QStringLiteral("a"));
    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 4);
    s.eraseChars(100);
    QCOMPARE(rowText(s, 0), QStringLiteral("abcd"));

    // Erased/inserted cells take the current background colour.
    s.setCurrentAttributes(bgAttr(2));
    s.moveCursorTo(0, 0);
    s.insertChars(1);
    QCOMPARE(s.line(0).cells[0].attr.bg, Color::indexed(2));
    QCOMPARE(s.line(0).cells[1].ch, U'a');
}

void Tst_terminalscreen::scrollUpDown()
{
    TerminalScreen s(5, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
                 QStringLiteral("5")});
    s.moveCursorTo(2, 2);
    s.scrollUp(2);
    QCOMPARE(rowText(s, 0), QStringLiteral("3"));
    QCOMPARE(rowText(s, 1), QStringLiteral("4"));
    QCOMPARE(rowText(s, 2), QStringLiteral("5"));
    QVERIFY(rowText(s, 3).isEmpty());
    QVERIFY(rowText(s, 4).isEmpty());
    QCOMPARE(s.scrollbackSize(), 0); // SU never pushes to scrollback
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(s.cursor().col, 2);

    s.scrollDown(1);
    QVERIFY(rowText(s, 0).isEmpty());
    QCOMPARE(rowText(s, 1), QStringLiteral("3"));
    QCOMPARE(rowText(s, 2), QStringLiteral("4"));
    QCOMPARE(rowText(s, 3), QStringLiteral("5"));

    s.scrollUp(100);
    for (int r = 0; r < 5; ++r) {
        QVERIFY(rowText(s, r).isEmpty());
    }
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.scrollUp(0);
    s.scrollDown(-1);
    QCOMPARE(content.count(), 0);
}

void Tst_terminalscreen::scrollRegionBehaviour()
{
    TerminalScreen s(5, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
                 QStringLiteral("5")});
    s.setScrollRegion(1, 3);
    QCOMPARE(s.scrollTop(), 1);
    QCOMPARE(s.scrollBottom(), 3);
    s.scrollUp(1);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QCOMPARE(rowText(s, 1), QStringLiteral("3"));
    QCOMPARE(rowText(s, 2), QStringLiteral("4"));
    QVERIFY(rowText(s, 3).isEmpty());
    QCOMPARE(rowText(s, 4), QStringLiteral("5"));
    s.scrollDown(1);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QVERIFY(rowText(s, 1).isEmpty());
    QCOMPARE(rowText(s, 2), QStringLiteral("3"));
    QCOMPARE(rowText(s, 3), QStringLiteral("4"));
    QCOMPARE(rowText(s, 4), QStringLiteral("5"));

    // Resetting to the full screen.
    s.setScrollRegion(0, 4);
    QCOMPARE(s.scrollTop(), 0);
    QCOMPARE(s.scrollBottom(), 4);
    // Bottom beyond the screen is clamped.
    s.setScrollRegion(2, 100);
    QCOMPARE(s.scrollTop(), 2);
    QCOMPARE(s.scrollBottom(), 4);
}

void Tst_terminalscreen::reverseIndexAndNextLine()
{
    TerminalScreen s(4, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4")});
    s.moveCursorTo(0, 2);
    s.reverseIndex();
    QVERIFY(rowText(s, 0).isEmpty());
    QCOMPARE(rowText(s, 1), QStringLiteral("1"));
    QCOMPARE(rowText(s, 2), QStringLiteral("2"));
    QCOMPARE(rowText(s, 3), QStringLiteral("3"));
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 2);
    s.moveCursorTo(2, 0);
    s.reverseIndex();
    QCOMPARE(s.cursor().row, 1);

    // RI inside a region scrolls only the region.
    s.setScrollRegion(1, 2);
    s.moveCursorTo(1, 0);
    s.reverseIndex();
    QVERIFY(rowText(s, 0).isEmpty());
    QVERIFY(rowText(s, 1).isEmpty());
    QCOMPARE(rowText(s, 2), QStringLiteral("1"));
    QCOMPARE(rowText(s, 3), QStringLiteral("3"));

    s.setScrollRegion(0, 3);
    s.moveCursorTo(2, 3);
    s.nextLine();
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 0);
    s.index();
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.scrollbackSize(), 1);
}

void Tst_terminalscreen::cursorLineOps()
{
    TerminalScreen s(10, 10);
    s.moveCursorTo(2, 5);
    s.cursorNextLine(2);
    QCOMPARE(s.cursor().row, 4);
    QCOMPARE(s.cursor().col, 0);
    s.moveCursorTo(4, 5);
    s.cursorPreviousLine(1);
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 0);
    s.cursorPreviousLine(100);
    QCOMPARE(s.cursor().row, 0);
    s.cursorNextLine(100);
    QCOMPARE(s.cursor().row, 9);
    s.setScrollRegion(2, 5);
    s.moveCursorTo(3, 3);
    s.cursorNextLine(50);
    QCOMPARE(s.cursor().row, 5);
    s.cursorPreviousLine(50);
    QCOMPARE(s.cursor().row, 2);
    // From outside the region: stop at the margin being approached, the screen edge otherwise.
    s.moveCursorTo(8, 4);
    s.cursorPreviousLine(50);
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(s.cursor().col, 0);
    s.moveCursorTo(0, 4);
    s.cursorNextLine(50);
    QCOMPARE(s.cursor().row, 5);
    QCOMPARE(s.cursor().col, 0);
    s.moveCursorTo(0, 0);
    s.cursorPreviousLine(5);
    QCOMPARE(s.cursor().row, 0);
    s.moveCursorTo(8, 0);
    s.cursorNextLine(5);
    QCOMPARE(s.cursor().row, 9);
}

void Tst_terminalscreen::repeatLastChar()
{
    TerminalScreen s(3, 5);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.repeatLastChar(3); // nothing written yet: no-op
    QCOMPARE(content.count(), 0);
    s.putChar(U'a');
    s.repeatLastChar(3);
    QCOMPARE(rowText(s, 0), QStringLiteral("aaaa"));
    QCOMPARE(content.count(), 2);
    s.repeatLastChar(3); // wraps onto the next line
    QCOMPARE(rowText(s, 0), QStringLiteral("aaaaa"));
    QCOMPARE(rowText(s, 1), QStringLiteral("aa"));
}

void Tst_terminalscreen::insertMode()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abc"));
    s.moveCursorTo(0, 0);
    s.setInsertMode(true);
    QVERIFY(s.insertMode());
    s.putChar(U'X');
    QCOMPARE(rowText(s, 0), QStringLiteral("Xabc"));
    QCOMPARE(s.cursor().col, 1);
    s.putText(QStringLiteral("YZ"));
    QCOMPARE(rowText(s, 0), QStringLiteral("XYZab")); // 'c' pushed off the end
    s.setInsertMode(false);
    s.moveCursorTo(0, 0);
    s.putChar(U'Q');
    QCOMPARE(rowText(s, 0), QStringLiteral("QYZab"));
}

void Tst_terminalscreen::saveRestoreCursor()
{
    TerminalScreen s(10, 10);
    Attributes a;
    a.set(Bold, true);
    a.fg = Color::rgb(1, 2, 3);
    s.setCurrentAttributes(a);
    s.moveCursorTo(3, 4);
    s.setAutoWrap(false);
    s.saveCursor();
    s.moveCursorTo(0, 0);
    s.resetAttributes();
    s.setAutoWrap(true);
    s.restoreCursor();
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 4);
    QCOMPARE(s.currentAttributes(), a);
    QVERIFY(!s.autoWrap());

    // Restoring after a resize clamps the position.
    s.saveCursor();
    s.resize(2, 3);
    s.restoreCursor();
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 2);

    // Nothing saved: home + default attributes.
    TerminalScreen fresh(5, 5);
    fresh.setCurrentAttributes(a);
    fresh.moveCursorTo(2, 2);
    fresh.restoreCursor();
    QCOMPARE(fresh.cursor().row, 0);
    QCOMPARE(fresh.cursor().col, 0);
    QCOMPARE(fresh.currentAttributes(), Attributes());
}

void Tst_terminalscreen::alternateScreenRoundTrip()
{
    TerminalScreen s(3, 10, 10);
    s.putText(QStringLiteral("primary"));
    s.moveCursorTo(1, 2);
    Attributes a;
    a.set(Italic, true);
    s.setCurrentAttributes(a);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);

    s.setAlternateScreen(true, true);
    QVERIFY(s.alternateScreenActive());
    QVERIFY(s.allDirty());
    QCOMPARE(content.count(), 1);
    QVERIFY(rowText(s, 0).isEmpty());
    QCOMPARE(s.cursor().row, 1); // 1049 keeps the cursor position
    QCOMPARE(s.cursor().col, 2);
    s.moveCursorTo(0, 0);
    s.resetAttributes();
    s.putText(QStringLiteral("alt"));
    s.moveCursorTo(2, 0);
    s.lineFeed();
    s.lineFeed();
    QCOMPARE(s.scrollbackSize(), 0); // no scrollback while the alternate screen is active
    QVERIFY(rowText(s, 0).isEmpty());

    // DECSC/DECRC on the alternate screen use their own slot.
    s.moveCursorTo(2, 5);
    s.saveCursor();
    s.moveCursorTo(0, 0);
    s.restoreCursor();
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(s.cursor().col, 5);

    s.setAlternateScreen(true, true); // already active: no-op
    QVERIFY(s.alternateScreenActive());

    s.setAlternateScreen(false, true);
    QVERIFY(!s.alternateScreenActive());
    QCOMPARE(rowText(s, 0), QStringLiteral("primary"));
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 2);
    QCOMPARE(s.currentAttributes(), a);
    QCOMPARE(s.scrollbackSize(), 0);
}

void Tst_terminalscreen::alternateScreenWithoutCursorSave()
{
    TerminalScreen s(3, 10, 10);
    s.putText(QStringLiteral("primary"));
    s.moveCursorTo(2, 3);
    s.setAlternateScreen(true, false);
    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("alt"));
    s.setAlternateScreen(false, false);
    QCOMPARE(rowText(s, 0), QStringLiteral("primary"));
    QCOMPARE(s.cursor().row, 0); // cursor left where the alternate screen put it
    QCOMPARE(s.cursor().col, 3);
}

void Tst_terminalscreen::resizeShrinkMovesLinesToScrollback()
{
    TerminalScreen s(4, 5, 10);
    QSignalSpy size(&s, &TerminalScreen::sizeChanged);
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4")});
    s.setScrollRegion(1, 2);
    s.moveCursorTo(3, 1);
    s.resize(2, 5);
    QCOMPARE(s.rows(), 2);
    QCOMPARE(s.cols(), 5);
    QCOMPARE(s.scrollbackSize(), 2);
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("1"));
    QCOMPARE(s.scrollbackLine(1).text(), QStringLiteral("2"));
    QCOMPARE(rowText(s, 0), QStringLiteral("3"));
    QCOMPARE(rowText(s, 1), QStringLiteral("4"));
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 1);
    QCOMPARE(s.scrollTop(), 0);
    QCOMPARE(s.scrollBottom(), 1);
    QVERIFY(s.allDirty());
    QCOMPARE(size.count(), 1);
    QCOMPARE(size.last().at(0).toInt(), 2);
    QCOMPARE(size.last().at(1).toInt(), 5);
    QCOMPARE(sbChanged.count(), 1);
    QCOMPARE(s.dirtyRows().size(), 2);

    // Same size: nothing happens.
    s.resize(2, 5);
    QCOMPARE(size.count(), 1);
}

void Tst_terminalscreen::resizeGrowPullsLinesBack()
{
    TerminalScreen s(4, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4")});
    s.moveCursorTo(3, 1);
    s.resize(2, 5);
    s.resize(4, 5);
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QCOMPARE(rowText(s, 1), QStringLiteral("2"));
    QCOMPARE(rowText(s, 2), QStringLiteral("3"));
    QCOMPARE(rowText(s, 3), QStringLiteral("4"));
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 1);

    // Growing further with an empty scrollback appends blank lines at the bottom.
    s.resize(6, 5);
    QCOMPARE(rowText(s, 3), QStringLiteral("4"));
    QVERIFY(rowText(s, 4).isEmpty());
    QVERIFY(rowText(s, 5).isEmpty());
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.dirtyRows().size(), 6);
}

void Tst_terminalscreen::resizeShrinkDropsBlankBottomLines()
{
    TerminalScreen s(4, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2")});
    s.moveCursorTo(0, 0);
    s.resize(2, 5);
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(rowText(s, 0), QStringLiteral("1"));
    QCOMPARE(rowText(s, 1), QStringLiteral("2"));
    QCOMPARE(s.cursor().row, 0);

    // Alternate screen: nothing goes to the scrollback on shrink.
    TerminalScreen alt(4, 5, 10);
    alt.setAlternateScreen(true);
    fillRows(alt, {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")});
    alt.moveCursorTo(3, 0);
    alt.resize(2, 5);
    QCOMPARE(alt.scrollbackSize(), 0);
    QCOMPARE(rowText(alt, 0), QStringLiteral("c"));
    QCOMPARE(rowText(alt, 1), QStringLiteral("d"));
    QCOMPARE(alt.cursor().row, 1);
    alt.setAlternateScreen(false);
    QCOMPARE(alt.rows(), 2);
    QCOMPARE(alt.line(0).length(), 5);
}

void Tst_terminalscreen::resizeInAlternateScreenPreservesPrimary()
{
    TerminalScreen s(4, 10, 10);
    fillRows(s, {QStringLiteral("l0"), QStringLiteral("l1"), QStringLiteral("l2"), QStringLiteral("prompt$")});
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 7);
    s.setAlternateScreen(true, true);
    s.putText(QStringLiteral("top")); // alternate content must never reach the scrollback
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);

    s.resize(2, 10);
    QCOMPARE(s.scrollbackSize(), 2); // primary l0, l1 moved to the scrollback
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("l0"));
    QCOMPARE(s.scrollbackLine(1).text(), QStringLiteral("l1"));
    QCOMPARE(sbChanged.count(), 1);

    // Grow back while still on the alternate screen: the lines are pulled back.
    s.resize(4, 10);
    QCOMPARE(s.scrollbackSize(), 0);
    s.setAlternateScreen(false, true);
    QCOMPARE(rowText(s, 0), QStringLiteral("l0"));
    QCOMPARE(rowText(s, 3), QStringLiteral("prompt$"));
    QCOMPARE(s.cursor().row, 3);
    QCOMPARE(s.cursor().col, 7);

    // Shrink and leave: the prompt line and cursor survive, older lines are in the scrollback.
    s.setAlternateScreen(true, true);
    s.resize(2, 10);
    s.setAlternateScreen(false, true);
    QCOMPARE(s.scrollbackSize(), 2);
    QCOMPARE(rowText(s, 0), QStringLiteral("l2"));
    QCOMPARE(rowText(s, 1), QStringLiteral("prompt$"));
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 7);
}

void Tst_terminalscreen::resizeColumns()
{
    TerminalScreen s(2, 5, 10);
    s.putText(QStringLiteral("abcde"));
    s.moveCursorTo(0, 4);
    s.resize(2, 3);
    QCOMPARE(s.cols(), 3);
    QCOMPARE(rowText(s, 0), QStringLiteral("abc"));
    QCOMPARE(s.line(0).length(), 3);
    QCOMPARE(s.cursor().col, 2);
    s.resize(2, 8);
    QCOMPARE(rowText(s, 0), QStringLiteral("abc"));
    QCOMPARE(s.line(0).length(), 8);
    QCOMPARE(s.line(1).length(), 8);
    QCOMPARE(s.line(0).cells[7].ch, U' ');

    // Default tab stops exist in the new columns.
    s.resize(2, 20);
    s.moveCursorTo(0, 9);
    s.tab();
    QCOMPARE(s.cursor().col, 16);

    // A wide character cut in half by truncation is blanked.
    TerminalScreen w(1, 4, 0);
    w.putText(QStringLiteral("a中"));
    w.resize(1, 2);
    QCOMPARE(w.line(0).cells[0].ch, U'a');
    QCOMPARE(w.line(0).cells[1].ch, U' ');
    QVERIFY(!w.line(0).cells[1].attr.has(WideLead));
}

void Tst_terminalscreen::wideCharPlacement()
{
    TerminalScreen s(3, 5);
    Attributes a;
    a.fg = Color::indexed(2);
    s.setCurrentAttributes(a);
    s.putChar(0x4E2D);
    const Cell& lead = s.line(0).cells[0];
    const Cell& trail = s.line(0).cells[1];
    QCOMPARE(lead.ch, char32_t(0x4E2D));
    QVERIFY(lead.attr.has(WideLead));
    QVERIFY(!lead.attr.has(WideTrail));
    QCOMPARE(lead.attr.fg, Color::indexed(2));
    QCOMPARE(trail.ch, char32_t(0));
    QVERIFY(trail.isWideTrail());
    QVERIFY(trail.isBlank());
    QCOMPARE(trail.attr.fg, Color::indexed(2));
    QCOMPARE(s.cursor().col, 2);
    QCOMPARE(rowText(s, 0), QStringLiteral("中"));

    s.putText(QStringLiteral("文x"));
    QCOMPARE(rowText(s, 0), QStringLiteral("中文x"));
    QCOMPARE(s.cursor().col, 4);
    QCOMPARE(s.currentAttributes().flags, quint16(NoAttr)); // Wide flags never leak into the SGR state
}

void Tst_terminalscreen::wideCharOverwrite()
{
    TerminalScreen s(3, 5);
    s.putChar(0x4E2D);
    s.moveCursorTo(0, 1);
    s.putChar(U'x'); // overwrite the trail: the lead is blanked
    QCOMPARE(s.line(0).cells[0].ch, U' ');
    QVERIFY(!s.line(0).cells[0].attr.has(WideLead));
    QCOMPARE(s.line(0).cells[1].ch, U'x');
    QCOMPARE(rowText(s, 0), QStringLiteral(" x"));

    s.moveCursorTo(0, 0);
    s.putChar(0x4E2D);
    s.moveCursorTo(0, 0);
    s.putChar(U'y'); // overwrite the lead: the trail is blanked
    QCOMPARE(s.line(0).cells[1].ch, U' ');
    QVERIFY(!s.line(0).cells[1].isWideTrail());
    QCOMPARE(rowText(s, 0), QStringLiteral("y"));

    // A wide char overwriting the trail of another wide char.
    s.moveCursorTo(1, 0);
    s.putChar(0x4E2D); // cols 0-1
    s.moveCursorTo(1, 1);
    s.putChar(0x6587); // cols 1-2
    QCOMPARE(s.line(1).cells[0].ch, U' ');
    QCOMPARE(s.line(1).cells[1].ch, char32_t(0x6587));
    QVERIFY(s.line(1).cells[2].isWideTrail());
    QCOMPARE(rowText(s, 1), QStringLiteral(" 文"));

    // Erasing through the middle of a wide char blanks both halves.
    s.moveCursorTo(2, 1);
    s.putChar(0x4E2D); // cols 1-2
    s.moveCursorTo(2, 2);
    s.eraseInLine(0);
    QCOMPARE(s.line(2).cells[1].ch, U' ');
    QVERIFY(rowText(s, 2).isEmpty());
    s.moveCursorTo(2, 1);
    s.putChar(0x4E2D);
    s.moveCursorTo(2, 1);
    s.eraseInLine(1); // erases 0..1 -> the trail at 2 goes too
    QVERIFY(rowText(s, 2).isEmpty());
}

void Tst_terminalscreen::wideCharAtLastColumn()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abcd"));
    s.putChar(0x4E2D); // only one column left: wrap first
    QCOMPARE(rowText(s, 0), QStringLiteral("abcd"));
    QVERIFY(s.line(0).wrapped);
    QCOMPARE(s.line(1).cells[0].ch, char32_t(0x4E2D));
    QVERIFY(s.line(1).cells[1].isWideTrail());
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 2);

    // Wide char written in the last two columns leaves the cursor pending on the last column.
    s.moveCursorTo(1, 3);
    s.putChar(0x6587);
    QCOMPARE(s.cursor().row, 1);
    QCOMPARE(s.cursor().col, 4);
    s.putChar(U'z');
    QCOMPARE(s.cursor().row, 2);
    QCOMPARE(rowText(s, 2), QStringLiteral("z"));
    QVERIFY(s.line(1).wrapped);

    // Autowrap off: placed in the last column as a narrow cell, no trail.
    TerminalScreen n(2, 5);
    n.setAutoWrap(false);
    n.moveCursorTo(0, 4);
    n.putChar(0x4E2D);
    QCOMPARE(n.line(0).cells[4].ch, char32_t(0x4E2D));
    QVERIFY(!n.line(0).cells[4].attr.has(WideLead));
    QCOMPARE(n.cursor().row, 0);
    QCOMPARE(n.cursor().col, 4);
    QVERIFY(!n.line(0).wrapped);
}

void Tst_terminalscreen::wideCharInsertMode()
{
    TerminalScreen s(2, 5);
    s.putText(QStringLiteral("abcd"));
    s.moveCursorTo(0, 0);
    s.setInsertMode(true);
    s.putChar(0x4E2D);
    QCOMPARE(rowText(s, 0), QStringLiteral("中abc"));
    QCOMPARE(s.cursor().col, 2);
    // Inserting so that a wide char would be split at the end blanks it.
    s.moveCursorTo(0, 0);
    s.putText(QStringLiteral("xx"));
    QCOMPARE(rowText(s, 0), QStringLiteral("xx中a"));
    s.moveCursorTo(0, 0);
    s.putChar(U'y');
    QCOMPARE(rowText(s, 0), QStringLiteral("yxx中"));
    s.moveCursorTo(0, 0);
    s.putChar(U'z'); // the wide char no longer fits: both halves are dropped/blanked
    QCOMPARE(rowText(s, 0), QStringLiteral("zyxx"));
    QVERIFY(!s.line(0).cells[4].attr.has(WideLead));
}

void Tst_terminalscreen::textRangeAndLineText()
{
    TerminalScreen s(3, 10);
    fillRows(s, {QStringLiteral("hello"), QStringLiteral("world")});
    QCOMPARE(s.lineText(0), QStringLiteral("hello"));
    QCOMPARE(s.lineText(1), QStringLiteral("world"));
    QCOMPARE(s.lineText(2), QString());
    QCOMPARE(s.textRange(0, 0, 1, 3), QStringLiteral("hello\nwor"));
    QCOMPARE(s.textRange(0, 1, 0, 4), QStringLiteral("ell"));
    QCOMPARE(s.textRange(0, 0, 0, 10), QStringLiteral("hello"));
    QCOMPARE(s.textRange(0, 0, 2, 10), QStringLiteral("hello\nworld\n"));
    QCOMPARE(s.textRange(0, 7, 1, 2), QStringLiteral("\nwo")); // start in trailing blanks
    QCOMPARE(s.textRange(0, 2, 0, 2), QString());

    // Wide characters copy as one code point; selecting the trail includes the char.
    s.moveCursorTo(2, 0);
    s.putText(QStringLiteral("a中b"));
    QCOMPARE(s.lineText(2), QStringLiteral("a中b"));
    QCOMPARE(s.textRange(2, 2, 2, 4), QStringLiteral("中b"));
    QCOMPARE(s.textRange(2, 1, 2, 2), QStringLiteral("中"));

    QCOMPARE(s.line(0).text(false).size(), 10);
    QCOMPARE(s.line(0).text(false), QStringLiteral("hello     "));
}

void Tst_terminalscreen::textRangeJoinsWrappedLines()
{
    TerminalScreen s(3, 5);
    s.putText(QStringLiteral("abcdefgh"));
    QVERIFY(s.line(0).wrapped);
    QCOMPARE(s.textRange(0, 0, 1, 3), QStringLiteral("abcdefgh"));
    QCOMPARE(s.textRange(0, 3, 1, 1), QStringLiteral("def"));
    // Spaces at the end of a wrapped line are real content.
    s.moveCursorTo(1, 0);
    s.eraseInLine(2);
    s.putText(QStringLiteral("ab  x"));
    QCOMPARE(s.textRange(0, 0, 2, 0), QStringLiteral("abcdeab  x\n"));
    QCOMPARE(s.textRange(1, 0, 1, 4), QStringLiteral("ab  "));
    QCOMPARE(s.textRange(0, 0, 0, 5), QStringLiteral("abcde"));
}

void Tst_terminalscreen::textRangeNormalisesAndClamps()
{
    TerminalScreen s(3, 10, 10);
    fillRows(s, {QStringLiteral("hello"), QStringLiteral("world")});
    QCOMPARE(s.textRange(1, 3, 0, 0), QStringLiteral("hello\nwor")); // reversed coordinates
    QCOMPARE(s.textRange(0, 4, 0, 1), QStringLiteral("ell"));
    QCOMPARE(s.textRange(-5, -5, 100, 100), QStringLiteral("hello\nworld\n"));
    QCOMPARE(s.textRange(0, 100, 0, 200), QString());
    // Scrollback lines are addressed by absolute index.
    s.moveCursorTo(2, 0);
    s.lineFeed();
    QCOMPARE(s.scrollbackSize(), 1);
    QCOMPARE(s.textRange(0, 0, 1, 5), QStringLiteral("hello\nworld"));
    QCOMPARE(s.lineText(0), QStringLiteral("hello"));
    QCOMPARE(s.lineText(1), QStringLiteral("world"));
}

void Tst_terminalscreen::wordBoundsAt()
{
    TerminalScreen s(2, 30);
    s.putText(QStringLiteral("  foo-bar.c  baz/qux ~/x_1"));
    int from = -1;
    int to = -1;
    s.wordBoundsAt(0, 4, from, to);
    QCOMPARE(from, 2);
    QCOMPARE(to, 11);
    s.wordBoundsAt(0, 2, from, to);
    QCOMPARE(from, 2);
    QCOMPARE(to, 11);
    s.wordBoundsAt(0, 10, from, to);
    QCOMPARE(from, 2);
    QCOMPARE(to, 11);
    s.wordBoundsAt(0, 0, from, to); // blank: just the cell
    QCOMPARE(from, 0);
    QCOMPARE(to, 1);
    s.wordBoundsAt(0, 15, from, to);
    QCOMPARE(from, 13);
    QCOMPARE(to, 20);
    s.wordBoundsAt(0, 24, from, to);
    QCOMPARE(from, 21);
    QCOMPARE(to, 26);
    s.wordBoundsAt(0, 29, from, to); // trailing blank at the end of the line
    QCOMPARE(from, 29);
    QCOMPARE(to, 30);
    s.wordBoundsAt(0, 500, from, to); // clamped
    QCOMPARE(from, 29);
    QCOMPARE(to, 30);

    // CJK: ideographs are letters, both halves belong to the word.
    s.moveCursorTo(1, 0);
    s.putText(QStringLiteral("x 中文abc y"));
    s.wordBoundsAt(1, 3, from, to); // trail of the first ideograph
    QCOMPARE(from, 2);
    QCOMPARE(to, 9);
    s.wordBoundsAt(1, 7, from, to);
    QCOMPARE(from, 2);
    QCOMPARE(to, 9);
}

void Tst_terminalscreen::dirtyTracking()
{
    TerminalScreen s(5, 10, 10);
    QVERIFY(s.isDirty());
    QVERIFY(s.allDirty());
    s.clearDirty();
    QVERIFY(!s.isDirty());
    QVERIFY(!s.allDirty());
    QCOMPARE(s.dirtyRows().count(true), 0);

    s.putChar(U'a');
    QVERIFY(s.isDirty());
    QVERIFY(!s.allDirty());
    QVERIFY(s.dirtyRows().testBit(0));
    QCOMPARE(s.dirtyRows().count(true), 1);

    s.clearDirty();
    s.moveCursorTo(3, 0); // old and new cursor rows must repaint
    QVERIFY(s.isDirty());
    QVERIFY(s.dirtyRows().testBit(0));
    QVERIFY(s.dirtyRows().testBit(3));
    QCOMPARE(s.dirtyRows().count(true), 2);

    s.clearDirty();
    s.eraseInLine(2);
    QVERIFY(s.dirtyRows().testBit(3));
    QCOMPARE(s.dirtyRows().count(true), 1);

    s.clearDirty();
    s.moveCursorTo(4, 0);
    s.clearDirty();
    s.lineFeed(); // scroll: everything
    QVERIFY(s.allDirty());

    s.clearDirty();
    s.resize(6, 10);
    QVERIFY(s.allDirty());
    QCOMPARE(s.dirtyRows().size(), 6);

    s.clearDirty();
    s.clearScreen();
    QVERIFY(s.allDirty());

    s.clearDirty();
    s.scrollUp(1);
    QVERIFY(s.allDirty());

    s.clearDirty();
    s.insertLines(1);
    QVERIFY(s.allDirty());

    s.clearDirty();
    s.setCursorVisible(false);
    QVERIFY(s.isDirty());
    QVERIFY(!s.allDirty());
    QVERIFY(s.dirtyRows().testBit(s.cursor().row));

    // Attribute changes alone do not dirty anything.
    s.clearDirty();
    s.setCurrentAttributes(bgAttr(1));
    s.resetAttributes();
    s.saveCursor();
    QVERIFY(!s.isDirty());
}

void Tst_terminalscreen::reset()
{
    TerminalScreen s(5, 20, 10);
    s.putText(QStringLiteral("some text"));
    s.moveCursorTo(4, 0);
    s.lineFeed();
    QCOMPARE(s.scrollbackSize(), 1);
    s.setScrollRegion(1, 3);
    s.setOriginMode(true);
    s.setAutoWrap(false);
    s.setInsertMode(true);
    s.setCurrentAttributes(bgAttr(3));
    s.clearAllTabStops();
    s.setCursorVisible(false);
    s.setAlternateScreen(true);
    s.putText(QStringLiteral("alt"));
    s.setTitle(QStringLiteral("t"));
    s.moveCursorTo(2, 2);
    s.saveCursor();

    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    s.reset();
    QCOMPARE(content.count(), 1);
    QCOMPARE(sbChanged.count(), 1);
    QCOMPARE(s.scrollbackSize(), 0);
    QVERIFY(!s.alternateScreenActive());
    for (int r = 0; r < 5; ++r) {
        QVERIFY(rowText(s, r).isEmpty());
    }
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    QVERIFY(s.cursorVisible());
    QCOMPARE(s.scrollTop(), 0);
    QCOMPARE(s.scrollBottom(), 4);
    QVERIFY(!s.originMode());
    QVERIFY(s.autoWrap());
    QVERIFY(!s.insertMode());
    QCOMPARE(s.currentAttributes(), Attributes());
    QVERIFY(s.allDirty());
    QCOMPARE(s.title(), QStringLiteral("t")); // RIS does not touch the window title
    s.tab();
    QCOMPARE(s.cursor().col, 8); // tabs every 8 columns again
    s.tab();
    QCOMPARE(s.cursor().col, 16);
    s.moveCursorTo(3, 3);
    s.restoreCursor(); // saved cursors were discarded
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    QCOMPARE(s.line(0).cells[0].attr.bg, Color());
}

void Tst_terminalscreen::clearScreenAndScrollback()
{
    TerminalScreen s(3, 5, 10);
    s.putText(QStringLiteral("a"));
    s.moveCursorTo(2, 0);
    s.lineFeed();
    s.putText(QStringLiteral("b"));
    QCOMPARE(s.scrollbackSize(), 1);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.clearScreen();
    QCOMPARE(content.count(), 1);
    QCOMPARE(s.scrollbackSize(), 1); // kept
    QVERIFY(rowText(s, 2).isEmpty());
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);

    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    s.clearScrollback();
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(sbChanged.count(), 1);
    QCOMPARE(content.count(), 2);
    s.clearScrollback(); // already empty: no signals
    QCOMPARE(sbChanged.count(), 1);
    QCOMPARE(content.count(), 2);
}

void Tst_terminalscreen::pushScreenToScrollback()
{
    TerminalScreen s(4, 5, 10);
    fillRows(s, {QStringLiteral("1"), QStringLiteral("2")});
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    s.pushScreenToScrollback();
    QCOMPARE(s.scrollbackSize(), 2); // trailing blank rows are not pushed
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("1"));
    QCOMPARE(s.scrollbackLine(1).text(), QStringLiteral("2"));
    for (int r = 0; r < 4; ++r) {
        QVERIFY(rowText(s, r).isEmpty());
    }
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    QCOMPARE(sbChanged.count(), 1);

    // The cursor row is pushed even when blank (an empty prompt line).
    s.moveCursorTo(1, 0);
    s.pushScreenToScrollback();
    QCOMPARE(s.scrollbackSize(), 4);

    // Alternate screen: just cleared.
    s.setAlternateScreen(true);
    s.putText(QStringLiteral("x"));
    s.pushScreenToScrollback();
    QCOMPARE(s.scrollbackSize(), 4);
    QVERIFY(rowText(s, 0).isEmpty());
}

void Tst_terminalscreen::setScrollbackMaxTrims()
{
    TerminalScreen s(2, 5, 10);
    for (int i = 0; i < 6; ++i) {
        s.putText(QString::number(i));
        s.nextLine();
        s.nextLine();
    }
    QCOMPARE(s.scrollbackSize(), 10);
    QSignalSpy sbChanged(&s, &TerminalScreen::scrollbackChanged);
    s.setScrollbackMax(3);
    QCOMPARE(s.scrollbackMax(), 3);
    QCOMPARE(s.scrollbackSize(), 3);
    QCOMPARE(s.scrollbackLine(2).text(), QStringLiteral("5"));
    QCOMPARE(sbChanged.count(), 1);
    s.setScrollbackMax(100); // growing does not emit
    QCOMPARE(sbChanged.count(), 1);
    s.setScrollbackMax(-5);
    QCOMPARE(s.scrollbackMax(), 0);
    QCOMPARE(s.scrollbackSize(), 0);
}

void Tst_terminalscreen::scrollbackDroppedCounts()
{
    TerminalScreen s(2, 5, 10);
    QCOMPARE(s.scrollbackDropped(), qint64(0));
    // 12 lines pushed into a 10-line buffer: the two oldest are dropped. (The first nextLine()
    // on the 2-row grid only moves to row 1; every following one scrolls and pushes a line.)
    for (int i = 0; i < 13; ++i) {
        s.putText(QString::number(i));
        s.nextLine();
    }
    QCOMPARE(s.scrollbackSize(), 10);
    QCOMPARE(s.scrollbackDropped(), qint64(2));
    QCOMPARE(s.scrollbackLine(0).text(), QStringLiteral("2"));

    s.setScrollbackMax(3);   // trims 7 more
    QCOMPARE(s.scrollbackSize(), 3);
    QCOMPARE(s.scrollbackDropped(), qint64(9));
    s.setScrollbackMax(100);   // growing drops nothing
    QCOMPARE(s.scrollbackDropped(), qint64(9));
    s.clearScrollback();   // clearing is not a trim
    QCOMPARE(s.scrollbackSize(), 0);
    QCOMPARE(s.scrollbackDropped(), qint64(9));
    s.reset();
    QCOMPARE(s.scrollbackDropped(), qint64(9));
}

void Tst_terminalscreen::titleAndBell()
{
    TerminalScreen s;
    QSignalSpy title(&s, &TerminalScreen::titleChanged);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.setTitle(QStringLiteral("board"));
    QCOMPARE(s.title(), QStringLiteral("board"));
    QCOMPARE(title.count(), 1);
    QCOMPARE(title.last().at(0).toString(), QStringLiteral("board"));
    s.setTitle(QStringLiteral("board"));
    QCOMPARE(title.count(), 1);
    QCOMPARE(content.count(), 0);

    QSignalSpy bell(&s, &TerminalScreen::bellRequested);
    s.bell();
    s.bell();
    QCOMPARE(bell.count(), 2);
}

void Tst_terminalscreen::cursorVisibility()
{
    TerminalScreen s;
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.setCursorVisible(false);
    QVERIFY(!s.cursorVisible());
    QVERIFY(!s.cursor().visible);
    QCOMPARE(content.count(), 1);
    s.setCursorVisible(false);
    QCOMPARE(content.count(), 1);
    s.setCursorVisible(true);
    QVERIFY(s.cursorVisible());
    QCOMPARE(content.count(), 2);
}

void Tst_terminalscreen::setScrollRegionValidation()
{
    TerminalScreen s(10, 10);
    s.moveCursorTo(4, 4);
    QSignalSpy content(&s, &TerminalScreen::contentChanged);
    s.setScrollRegion(3, 3);
    s.setScrollRegion(5, 2);
    QCOMPARE(content.count(), 0);
    QCOMPARE(s.scrollTop(), 0);
    QCOMPARE(s.scrollBottom(), 9);
    QCOMPARE(s.cursor().row, 4);
    s.setScrollRegion(2, 6);
    QCOMPARE(content.count(), 1);
    QCOMPARE(s.scrollTop(), 2);
    QCOMPARE(s.scrollBottom(), 6);
    QCOMPARE(s.cursor().row, 0);
    QCOMPARE(s.cursor().col, 0);
    s.setScrollRegion(-3, 100);
    QCOMPARE(s.scrollTop(), 0);
    QCOMPARE(s.scrollBottom(), 9);
}

QTEST_GUILESS_MAIN(Tst_terminalscreen)
#include "tst_terminalscreen.moc"
