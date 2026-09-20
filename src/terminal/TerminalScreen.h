#pragma once

#include <QObject>
#include <QVector>
#include <QList>
#include <QString>
#include <QBitArray>

#include "terminal/TerminalTypes.h"

/**
 * The terminal's screen model: a rows x cols grid of cells plus a scrollback buffer,
 * cursor, current SGR attributes, tab stops, scroll region and DEC modes.
 *
 * This class knows nothing about escape sequences (AnsiParser translates them into the
 * operations below) and nothing about painting (TerminalWidget reads lines and cursor).
 * It is pure Qt Core and fully unit-tested (tests/tst_terminalscreen.cpp).
 *
 * Coordinates are 0-based. "Absolute line" indices address the scrollback followed by
 * the visible screen: 0 .. scrollbackSize()-1 are history (oldest first),
 * scrollbackSize() .. scrollbackSize()+rows()-1 are the visible rows.
 *
 * Scrolling semantics: when the cursor is on the bottom margin of the scroll region and
 * a line feed occurs, the region scrolls up; if the region is the full screen and the
 * alternate screen is not active, the top line is pushed to the scrollback (capped at
 * scrollbackMax(), oldest dropped). insertLines/deleteLines/scrollUp/scrollDown never
 * push to scrollback (they are editing operations, like xterm).
 *
 * Wide characters: putChar() of a width-2 code point writes a WideLead cell followed by a
 * WideTrail placeholder; if only one column remains, the line is wrapped first (or the
 * character is placed at the last column and the trail dropped when autowrap is off).
 * Overwriting either half of a wide char blanks the other half.
 *
 * Every mutating call sets a dirty flag and marks the affected rows in dirtyRows(); the
 * renderer clears them after painting. Signals are emitted for the events a widget cares
 * about; heavy output should be coalesced by the widget (contentChanged is emitted at
 * most once per mutating call, never in loops).
 */
class TerminalScreen : public QObject
{
    Q_OBJECT
public:
    explicit TerminalScreen(int rows = 24, int cols = 80, int scrollbackMax = 10000, QObject* parent = nullptr);

    // ---- Geometry -------------------------------------------------------------------
    int rows() const;
    int cols() const;
    /// Resize the grid. Columns: lines are truncated or padded (no reflow). Rows: when
    /// shrinking, lines above the cursor are moved into the scrollback first so the cursor
    /// stays visible; when growing, lines are pulled back from the scrollback while any
    /// exist, otherwise blank lines are appended at the bottom. Scroll region is reset to
    /// full screen. Cursor is clamped. Emits sizeChanged() and contentChanged().
    void resize(int rows, int cols);
    int scrollbackMax() const;
    void setScrollbackMax(int lines);   ///< trims the buffer if needed

    // ---- Read access ----------------------------------------------------------------
    const Terminal::Line& line(int row) const;              ///< visible row 0..rows()-1
    int scrollbackSize() const;
    const Terminal::Line& scrollbackLine(int index) const;  ///< 0 = oldest
    int totalLines() const;                                 ///< scrollbackSize() + rows()
    const Terminal::Line& absoluteLine(int index) const;
    Terminal::Cursor cursor() const;
    bool cursorVisible() const;
    Terminal::Attributes currentAttributes() const;
    bool alternateScreenActive() const;
    bool autoWrap() const;
    bool originMode() const;
    bool insertMode() const;
    int scrollTop() const;      ///< inclusive, 0-based
    int scrollBottom() const;   ///< inclusive, 0-based
    QString title() const;
    QString lineText(int absoluteIndex) const;
    /// Text between two absolute positions (start inclusive, end exclusive on the end
    /// column). Lines are joined with '\n' unless the earlier line is `wrapped`; trailing
    /// blanks of each line are trimmed. Coordinates are clamped.
    QString textRange(int startLine, int startCol, int endLine, int endCol) const;
    /// Word boundaries for double-click selection: expands around `col` in absolute line
    /// `line` over "word characters" (alnum, '_', '-', '.', '/', '~'). Returns [from, to).
    void wordBoundsAt(int line, int col, int& from, int& to) const;

    // ---- Dirty tracking -------------------------------------------------------------
    bool isDirty() const;
    bool allDirty() const;             ///< whole screen changed (scroll, resize, clear...)
    QBitArray dirtyRows() const;       ///< size rows(); meaningful when !allDirty()
    void clearDirty();

    // ---- Attribute / character output ----------------------------------------------
    void setCurrentAttributes(const Terminal::Attributes& attr);
    void resetAttributes();            ///< SGR 0
    /// Write one printable code point at the cursor with currentAttributes(), advancing
    /// the cursor; handles pending-wrap (xterm semantics: the cursor stays on the last
    /// column after writing there and wraps on the next printable), wide chars and insert
    /// mode. Control characters (< 0x20, 0x7F) are ignored - AnsiParser handles them.
    void putChar(char32_t codePoint);
    /// Convenience: iterate code points (surrogate pairs combined) through putChar().
    void putText(const QString& text);

    // ---- C0 controls ----------------------------------------------------------------
    void lineFeed();          ///< LF/VT/FF: down one row, scroll at bottom margin
    void carriageReturn();    ///< to column 0 (or left margin in origin mode - we have no left margin: 0)
    void backspace();         ///< cursor left one (no wrap to previous line), never erases
    void tab();               ///< to next tab stop or last column
    void bell();              ///< emits bellRequested()
    void index();             ///< IND: same as lineFeed
    void reverseIndex();      ///< RI: up one row, scroll region down at top margin
    void nextLine();          ///< NEL: carriageReturn + lineFeed

    // ---- Cursor movement (clamped to screen / to scroll region in origin mode) --------
    void moveCursorTo(int row, int col);            ///< CUP/HVP (0-based; in origin mode row is relative to scrollTop)
    void moveCursorBy(int dRow, int dCol);          ///< CUU/CUD/CUF/CUB (stays inside the scroll region vertically when inside it)
    void setCursorRow(int row);                     ///< VPA
    void setCursorColumn(int col);                  ///< CHA
    void cursorNextLine(int n);                     ///< CNL
    void cursorPreviousLine(int n);                 ///< CPL
    void saveCursor();                              ///< DECSC: position + attributes + origin/autowrap state
    void restoreCursor();                           ///< DECRC
    void setCursorVisible(bool visible);            ///< DECTCEM

    // ---- Erase / edit ---------------------------------------------------------------
    /// ED: 0 = cursor to end, 1 = start to cursor, 2 = whole screen, 3 = scrollback only.
    /// Erased cells take the current background colour (bce) but no other attributes.
    void eraseInDisplay(int mode);
    /// EL: 0 = cursor to end of line, 1 = start to cursor, 2 = whole line.
    void eraseInLine(int mode);
    void insertLines(int n);      ///< IL  (within scroll region, at cursor row)
    void deleteLines(int n);      ///< DL
    void insertChars(int n);      ///< ICH (shift right within the line)
    void deleteChars(int n);      ///< DCH
    void eraseChars(int n);       ///< ECH
    void scrollUp(int n);         ///< SU  (region scrolls, no scrollback push)
    void scrollDown(int n);       ///< SD
    void repeatLastChar(int n);   ///< REP

    // ---- Modes -----------------------------------------------------------------------
    /// DECSTBM: 0-based inclusive rows; invalid (top >= bottom) -> ignored; both 0/rows-1 resets. Moves cursor home.
    void setScrollRegion(int top, int bottom);
    void setAutoWrap(bool on);          ///< DECAWM (?7), default on
    void setOriginMode(bool on);        ///< DECOM  (?6), default off; homes the cursor
    void setInsertMode(bool on);        ///< IRM    (4),  default off
    /// ?1049 / ?47 / ?1047: switch to a blank alternate screen (no scrollback while active)
    /// and back, restoring the primary screen contents. ?1049 also saves/restores the cursor.
    void setAlternateScreen(bool on, bool saveCursor = true);
    void setTitle(const QString& title);

    // ---- Tabs ------------------------------------------------------------------------
    void setTabStop();                  ///< HTS at cursor column
    void clearTabStop();                ///< TBC 0
    void clearAllTabStops();            ///< TBC 3
    void resetTabStops();               ///< every 8 columns

    // ---- Whole-screen ---------------------------------------------------------------
    void reset();             ///< RIS: clear screen + scrollback, home cursor, default attributes/modes/tabs
    void clearScreen();       ///< erase all cells, keep scrollback, home cursor (menu "Clear")
    void clearScrollback();
    /// Move the visible screen contents into the scrollback and clear the screen (like
    /// pressing Ctrl+L in a shell would appear); used by "Clear" so nothing is lost.
    void pushScreenToScrollback();

signals:
    void contentChanged();
    void cursorMoved(int row, int col);
    void bellRequested();
    void sizeChanged(int rows, int cols);
    void titleChanged(const QString& title);
    void scrollbackChanged(int size);

private:
    struct SavedCursor
    {
        Terminal::Cursor cursor;
        Terminal::Attributes attr;
        bool originMode = false;
        bool autoWrap = true;
        bool valid = false;
    };

    void scrollRegionUp(int n, bool pushToScrollback);
    void scrollRegionDown(int n);
    void markDirty(int row);
    void markAllDirty();
    Terminal::Line blankLine() const;
    Terminal::Cell blankCell() const;   ///< ' ' with current bg colour only
    void clampCursor();

    // Internal (non-emitting) helpers so that every public call emits contentChanged() once.
    void notifyChanged(const Terminal::Cursor& before);    ///< emits contentChanged/cursorMoved/scrollbackChanged
    void doLineFeed();                                     ///< lineFeed() without signals
    void doPutChar(char32_t codePoint);                    ///< putChar() without signals
    void eraseCells(int row, int from, int to);            ///< [from, to) -> blankCell(); repairs split wide chars
    void fixWideCells(Terminal::Line& line) const;         ///< blanks orphaned WideLead / WideTrail halves
    void fitLineToColumns(Terminal::Line& line, int cols) const;   ///< truncate or pad a line
    void trimScrollback();                                 ///< drop oldest lines above scrollbackMax()
    SavedCursor& activeSavedCursor();                      ///< primary or alternate DECSC slot

    int m_rows;
    int m_cols;
    int m_scrollbackMax;
    QVector<Terminal::Line> m_screen;         ///< visible rows
    QVector<Terminal::Line> m_savedScreen;    ///< primary screen while alternate is active
    QList<Terminal::Line> m_scrollback;
    Terminal::Cursor m_cursor;
    Terminal::Attributes m_attr;
    SavedCursor m_savedCursor;
    SavedCursor m_savedCursorAlt;
    QBitArray m_tabStops;
    int m_scrollTop = 0;
    int m_scrollBottom = 0;
    bool m_autoWrap = true;
    bool m_originMode = false;
    bool m_insertMode = false;
    bool m_pendingWrap = false;
    bool m_alternate = false;
    char32_t m_lastChar = 0;
    QString m_title;
    bool m_dirty = true;
    bool m_allDirty = true;
    QBitArray m_dirtyRows;
    bool m_scrollbackDirty = false;   ///< scrollbackChanged() pending for the current public call
};
