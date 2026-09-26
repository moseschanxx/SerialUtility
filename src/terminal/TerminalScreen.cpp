#include "terminal/TerminalScreen.h"

#include "terminal/CharWidth.h"

#include <algorithm>
#include <utility>

using Terminal::Attributes;
using Terminal::Cell;
using Terminal::Cursor;
using Terminal::Line;

namespace {

int clampInt(int value, int lo, int hi)
{
    return std::max(lo, std::min(value, hi));
}

void appendCodePoint(QString& out, char32_t cp)
{
    if (QChar::requiresSurrogates(cp)) {
        out.append(QChar(QChar::highSurrogate(cp)));
        out.append(QChar(QChar::lowSurrogate(cp)));
    } else {
        out.append(QChar(static_cast<char16_t>(cp)));
    }
}

bool isWordChar(char32_t cp)
{
    if (cp == U'_' || cp == U'-' || cp == U'.' || cp == U'/' || cp == U'~') {
        return true;
    }
    return QChar::isLetterOrNumber(cp);
}

bool isBlankLine(const Line& line)
{
    return std::all_of(line.cells.cbegin(), line.cells.cend(), [](const Cell& c) { return c.isBlank(); });
}

/// Index one past the last non-blank cell of a line.
int logicalLength(const Line& line)
{
    int end = static_cast<int>(line.cells.size());
    while (end > 0 && line.cells[end - 1].isBlank()) {
        --end;
    }
    return end;
}

} // namespace

// ---- Terminal::Line -----------------------------------------------------------------------

QString Terminal::Line::text(bool trimRight) const
{
    const int end = trimRight ? logicalLength(*this) : static_cast<int>(cells.size());
    QString out;
    out.reserve(end);
    for (int i = 0; i < end; ++i) {
        const Cell& c = cells[i];
        if (c.ch == 0) {
            continue; // wide trail placeholder
        }
        appendCodePoint(out, c.ch);
    }
    return out;
}

// ---- Construction / geometry --------------------------------------------------------------

TerminalScreen::TerminalScreen(int rows, int cols, int scrollbackMax, QObject* parent)
    : QObject(parent)
    , m_rows(std::max(1, rows))
    , m_cols(std::max(1, cols))
    , m_scrollbackMax(std::max(0, scrollbackMax))
{
    m_screen.reserve(m_rows);
    for (int r = 0; r < m_rows; ++r) {
        m_screen.append(blankLine());
    }
    m_scrollBottom = m_rows - 1;
    m_tabStops.resize(m_cols);
    resetTabStops();
    m_dirtyRows.resize(m_rows);
    markAllDirty();
}

int TerminalScreen::rows() const
{
    return m_rows;
}

int TerminalScreen::cols() const
{
    return m_cols;
}

void TerminalScreen::resize(int rows, int cols)
{
    rows = std::max(1, rows);
    cols = std::max(1, cols);
    if (rows == m_rows && cols == m_cols) {
        return;
    }
    const Cursor before = m_cursor;

    // -- columns: truncate or pad every visible line (no reflow) ------------------------
    if (cols != m_cols) {
        for (Line& l : m_screen) {
            fitLineToColumns(l, cols);
        }
        for (Line& l : m_savedScreen) {
            fitLineToColumns(l, cols);
        }
        QBitArray tabs(cols, false);
        const int keep = std::min(cols, m_cols);
        for (int c = 0; c < keep; ++c) {
            if (m_tabStops.testBit(c)) {
                tabs.setBit(c);
            }
        }
        for (int c = ((m_cols + 7) / 8) * 8; c < cols; c += 8) {
            tabs.setBit(c);
        }
        m_tabStops = tabs;
        m_cols = cols;
    }

    // -- rows -------------------------------------------------------------------------
    if (rows < m_rows) {
        int excess = m_rows - rows;
        // Lines above the cursor go to the scrollback first so the cursor stays visible.
        int toMove = std::min(excess, std::max(0, m_cursor.row - rows + 1));
        for (int i = 0; i < toMove; ++i) {
            Line l = m_screen.takeFirst();
            if (!m_alternate && m_scrollbackMax > 0) {
                m_scrollback.append(std::move(l));
                m_scrollbackDirty = true;
            }
        }
        m_cursor.row -= toMove;
        excess -= toMove;
        // The remaining excess is dropped from the bottom.
        m_screen.resize(m_screen.size() - excess);
        trimScrollback();
    } else if (rows > m_rows) {
        int needed = rows - m_rows;
        while (needed > 0 && !m_alternate && !m_scrollback.isEmpty()) {
            Line l = m_scrollback.takeLast();
            fitLineToColumns(l, m_cols);
            m_screen.prepend(std::move(l));
            ++m_cursor.row;
            m_scrollbackDirty = true;
            --needed;
        }
        while (needed-- > 0) {
            m_screen.append(blankLine());
        }
    }
    if (m_alternate) {
        // The saved primary screen follows the same policy as the visible grid, anchored
        // on the ?1049 saved cursor (or on the last non-blank line when ?47/?1047 saved none).
        int anchor = 0;
        if (m_savedCursor.valid) {
            anchor = m_savedCursor.cursor.row;
        } else {
            for (int r = static_cast<int>(m_savedScreen.size()) - 1; r >= 0; --r) {
                if (!m_savedScreen[r].text().isEmpty()) {
                    anchor = r;
                    break;
                }
            }
        }
        const int excess = static_cast<int>(m_savedScreen.size()) - rows;
        if (excess > 0) {
            const int toMove = std::min(excess, std::max(0, anchor - rows + 1));
            for (int i = 0; i < toMove; ++i) {
                Line l = m_savedScreen.takeFirst();
                if (m_scrollbackMax > 0) {
                    m_scrollback.append(std::move(l));
                    m_scrollbackDirty = true;
                }
            }
            if (m_savedCursor.valid) {
                m_savedCursor.cursor.row = std::max(0, m_savedCursor.cursor.row - toMove);
            }
            m_savedScreen.resize(m_savedScreen.size() - (excess - toMove));
            trimScrollback();
        } else {
            int needed = -excess;
            while (needed > 0 && !m_scrollback.isEmpty()) {
                Line l = m_scrollback.takeLast();
                fitLineToColumns(l, m_cols);
                m_savedScreen.prepend(std::move(l));
                if (m_savedCursor.valid) {
                    ++m_savedCursor.cursor.row;
                }
                m_scrollbackDirty = true;
                --needed;
            }
            while (needed-- > 0) {
                m_savedScreen.append(blankLine());
            }
        }
    }
    m_rows = rows;

    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_pendingWrap = false;
    clampCursor();
    m_dirtyRows.resize(m_rows);
    markAllDirty();
    emit sizeChanged(m_rows, m_cols);
    notifyChanged(before);
}

int TerminalScreen::scrollbackMax() const
{
    return m_scrollbackMax;
}

void TerminalScreen::setScrollbackMax(int lines)
{
    m_scrollbackMax = std::max(0, lines);
    const qsizetype oldSize = m_scrollback.size();
    trimScrollback();
    if (m_scrollback.size() != oldSize) {
        m_scrollbackDirty = true;
        markAllDirty();
        notifyChanged(m_cursor);
    }
}

// ---- Read access ------------------------------------------------------------------------

const Line& TerminalScreen::line(int row) const
{
    return m_screen[clampInt(row, 0, m_rows - 1)];
}

int TerminalScreen::scrollbackSize() const
{
    return static_cast<int>(m_scrollback.size());
}

qint64 TerminalScreen::scrollbackDropped() const
{
    return m_scrollbackDropped;
}

const Line& TerminalScreen::scrollbackLine(int index) const
{
    static const Line empty;
    if (m_scrollback.isEmpty()) {
        return empty;
    }
    return m_scrollback[clampInt(index, 0, scrollbackSize() - 1)];
}

int TerminalScreen::totalLines() const
{
    return scrollbackSize() + m_rows;
}

const Line& TerminalScreen::absoluteLine(int index) const
{
    index = clampInt(index, 0, totalLines() - 1);
    const int sb = scrollbackSize();
    if (index < sb) {
        return m_scrollback[index];
    }
    return m_screen[index - sb];
}

Cursor TerminalScreen::cursor() const
{
    return m_cursor;
}

bool TerminalScreen::cursorVisible() const
{
    return m_cursor.visible;
}

Attributes TerminalScreen::currentAttributes() const
{
    return m_attr;
}

bool TerminalScreen::alternateScreenActive() const
{
    return m_alternate;
}

bool TerminalScreen::autoWrap() const
{
    return m_autoWrap;
}

bool TerminalScreen::originMode() const
{
    return m_originMode;
}

bool TerminalScreen::insertMode() const
{
    return m_insertMode;
}

int TerminalScreen::scrollTop() const
{
    return m_scrollTop;
}

int TerminalScreen::scrollBottom() const
{
    return m_scrollBottom;
}

QString TerminalScreen::title() const
{
    return m_title;
}

QString TerminalScreen::lineText(int absoluteIndex) const
{
    return absoluteLine(absoluteIndex).text(true);
}

QString TerminalScreen::textRange(int startLine, int startCol, int endLine, int endCol) const
{
    const int total = totalLines();
    startLine = clampInt(startLine, 0, total - 1);
    endLine = clampInt(endLine, 0, total - 1);
    if (startLine > endLine || (startLine == endLine && startCol > endCol)) {
        std::swap(startLine, endLine);
        std::swap(startCol, endCol);
    }

    QString out;
    for (int l = startLine; l <= endLine; ++l) {
        const Line& ln = absoluteLine(l);
        const int len = ln.length();
        int from = (l == startLine) ? clampInt(startCol, 0, len) : 0;
        int to = (l == endLine) ? clampInt(endCol, 0, len) : len;
        // Starting on the second half of a wide character selects the whole character.
        if (from > 0 && from < len && ln.cells[from].isWideTrail()) {
            --from;
        }
        // Trailing blanks are trimmed on lines that end there; a soft-wrapped line is all content.
        if (!ln.wrapped) {
            to = std::min(to, logicalLength(ln));
        }
        for (int c = from; c < to; ++c) {
            const Cell& cell = ln.cells[c];
            if (cell.ch != 0) {
                appendCodePoint(out, cell.ch);
            }
        }
        if (l != endLine && !ln.wrapped) {
            out.append(QLatin1Char('\n'));
        }
    }
    return out;
}

void TerminalScreen::wordBoundsAt(int line, int col, int& from, int& to) const
{
    const Line& ln = absoluteLine(line);
    const int len = ln.length();
    if (len == 0) {
        from = 0;
        to = 0;
        return;
    }
    col = clampInt(col, 0, len - 1);

    const auto cellIsWord = [&ln](int c) {
        const Cell& cell = ln.cells[c];
        if (cell.isWideTrail() && c > 0) {
            return isWordChar(ln.cells[c - 1].ch);
        }
        return isWordChar(cell.ch);
    };

    if (!cellIsWord(col)) {
        // Not a word character: select just this cell (both halves of a wide char).
        from = col;
        to = col + 1;
        if (ln.cells[col].isWideTrail() && col > 0) {
            --from;
        } else if (ln.cells[col].attr.has(Terminal::WideLead) && col + 1 < len) {
            ++to;
        }
        return;
    }
    from = col;
    while (from > 0 && cellIsWord(from - 1)) {
        --from;
    }
    to = col + 1;
    while (to < len && cellIsWord(to)) {
        ++to;
    }
}

// ---- Dirty tracking -----------------------------------------------------------------------

bool TerminalScreen::isDirty() const
{
    return m_dirty;
}

bool TerminalScreen::allDirty() const
{
    return m_allDirty;
}

QBitArray TerminalScreen::dirtyRows() const
{
    return m_dirtyRows;
}

void TerminalScreen::clearDirty()
{
    m_dirty = false;
    m_allDirty = false;
    m_dirtyRows.fill(false);
}

void TerminalScreen::markDirty(int row)
{
    m_dirty = true;
    if (row >= 0 && row < m_dirtyRows.size()) {
        m_dirtyRows.setBit(row);
    }
}

void TerminalScreen::markAllDirty()
{
    m_dirty = true;
    m_allDirty = true;
    m_dirtyRows.fill(true);
}

void TerminalScreen::notifyChanged(const Cursor& before)
{
    const bool moved = before.row != m_cursor.row || before.col != m_cursor.col;
    if (moved) {
        markDirty(before.row);
        markDirty(m_cursor.row);
    }
    m_dirty = true;
    if (m_batchDepth > 0) {
        m_batchChanged = true;   // endBatch() emits once for the whole batch
        return;
    }
    emitChanged(moved);
}

void TerminalScreen::emitChanged(bool moved)
{
    if (m_scrollbackDirty) {
        m_scrollbackDirty = false;
        emit scrollbackChanged(scrollbackSize());
    }
    emit contentChanged();
    if (moved) {
        emit cursorMoved(m_cursor.row, m_cursor.col);
    }
}

void TerminalScreen::beginBatch()
{
    if (m_batchDepth++ == 0) {
        m_batchCursor = m_cursor;
        m_batchChanged = false;
    }
}

void TerminalScreen::endBatch()
{
    if (m_batchDepth <= 0) {
        return;
    }
    if (--m_batchDepth > 0 || !m_batchChanged) {
        return;
    }
    m_batchChanged = false;
    emitChanged(m_batchCursor.row != m_cursor.row || m_batchCursor.col != m_cursor.col);
}

// ---- Attribute / character output ---------------------------------------------------------

void TerminalScreen::setCurrentAttributes(const Attributes& attr)
{
    m_attr = attr;
    m_attr.set(Terminal::WideLead, false);
    m_attr.set(Terminal::WideTrail, false);
}

void TerminalScreen::resetAttributes()
{
    m_attr = Attributes();
}

void TerminalScreen::putChar(char32_t codePoint)
{
    if (Terminal::isControl(codePoint) || Terminal::charWidth(codePoint) == 0) {
        return;
    }
    const Cursor before = m_cursor;
    doPutChar(codePoint);
    notifyChanged(before);
}

void TerminalScreen::putText(const QString& text)
{
    if (text.isEmpty()) {
        return;
    }
    const Cursor before = m_cursor;
    bool changed = false;
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        char32_t cp = text[i].unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < n && QChar::isLowSurrogate(text[i + 1].unicode())) {
            cp = QChar::surrogateToUcs4(text[i].unicode(), text[i + 1].unicode());
            ++i;
        }
        if (Terminal::isControl(cp) || Terminal::charWidth(cp) == 0) {
            continue;
        }
        doPutChar(cp);
        changed = true;
    }
    if (changed) {
        notifyChanged(before);
    }
}

void TerminalScreen::doPutChar(char32_t codePoint)
{
    int width = Terminal::charWidth(codePoint);
    if (width == 2 && m_cols < 2) {
        width = 1; // a one-column screen cannot hold a double-width cell: place it as narrow
    }

    if (m_pendingWrap) {
        // xterm semantics: the previous character filled the last column; wrap now.
        m_screen[m_cursor.row].wrapped = true;
        markDirty(m_cursor.row);
        m_cursor.col = 0;
        doLineFeed();
        m_pendingWrap = false;
    }

    if (width == 2 && m_cursor.col == m_cols - 1) {
        if (m_autoWrap) {
            // Only one column left: blank it and wrap before placing the wide character.
            eraseCells(m_cursor.row, m_cursor.col, m_cursor.col + 1);
            m_screen[m_cursor.row].wrapped = true;
            m_cursor.col = 0;
            doLineFeed();
        } else {
            // Autowrap off: place it in the last column as a narrow cell, trail dropped.
            eraseCells(m_cursor.row, m_cursor.col, m_cursor.col + 1);
            Cell cell;
            cell.ch = codePoint;
            cell.attr = m_attr;
            m_screen[m_cursor.row].cells[m_cursor.col] = cell;
            m_lastChar = codePoint;
            markDirty(m_cursor.row);
            return;
        }
    }

    Line& line = m_screen[m_cursor.row];
    const int col = m_cursor.col;

    if (m_insertMode) {
        if (line.cells[col].isWideTrail()) {
            eraseCells(m_cursor.row, col, col + 1);
        }
        for (int c = m_cols - 1; c >= col + width; --c) {
            line.cells[c] = line.cells[c - width];
        }
        for (int c = col; c < col + width && c < m_cols; ++c) {
            line.cells[c] = blankCell();
        }
    } else {
        // Overwriting either half of a wide character blanks the other half.
        eraseCells(m_cursor.row, col, col + width);
    }

    Cell lead;
    lead.ch = codePoint;
    lead.attr = m_attr;
    lead.attr.set(Terminal::WideLead, width == 2);
    line.cells[col] = lead;
    if (width == 2) {
        Cell trail;
        trail.ch = 0;
        trail.attr = m_attr;
        trail.attr.set(Terminal::WideTrail, true);
        line.cells[col + 1] = trail;
    }
    if (m_insertMode) {
        fixWideCells(line);
    }
    m_lastChar = codePoint;
    markDirty(m_cursor.row);

    const int next = col + width;
    if (next >= m_cols) {
        m_cursor.col = m_cols - 1;
        if (m_autoWrap) {
            m_pendingWrap = true;
        }
    } else {
        m_cursor.col = next;
    }
}

// ---- C0 controls --------------------------------------------------------------------------

void TerminalScreen::doLineFeed()
{
    m_pendingWrap = false;
    if (m_cursor.row == m_scrollBottom) {
        const bool push = !m_alternate && m_scrollTop == 0 && m_scrollBottom == m_rows - 1;
        scrollRegionUp(1, push);
    } else if (m_cursor.row < m_rows - 1) {
        ++m_cursor.row;
    }
}

void TerminalScreen::lineFeed()
{
    const Cursor before = m_cursor;
    doLineFeed();
    notifyChanged(before);
}

void TerminalScreen::carriageReturn()
{
    const Cursor before = m_cursor;
    m_cursor.col = 0;
    m_pendingWrap = false;
    notifyChanged(before);
}

void TerminalScreen::backspace()
{
    const Cursor before = m_cursor;
    if (m_cursor.col > 0) {
        --m_cursor.col;
    }
    m_pendingWrap = false;
    notifyChanged(before);
}

void TerminalScreen::tab()
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    int c = m_cursor.col + 1;
    while (c < m_cols && !m_tabStops.testBit(c)) {
        ++c;
    }
    m_cursor.col = std::min(c, m_cols - 1);
    notifyChanged(before);
}

void TerminalScreen::bell()
{
    emit bellRequested();
}

void TerminalScreen::index()
{
    lineFeed();
}

void TerminalScreen::reverseIndex()
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    if (m_cursor.row == m_scrollTop) {
        scrollRegionDown(1);
    } else if (m_cursor.row > 0) {
        --m_cursor.row;
    }
    notifyChanged(before);
}

void TerminalScreen::nextLine()
{
    const Cursor before = m_cursor;
    m_cursor.col = 0;
    doLineFeed();
    notifyChanged(before);
}

// ---- Cursor movement ----------------------------------------------------------------------

void TerminalScreen::moveCursorTo(int row, int col)
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    if (m_originMode) {
        m_cursor.row = clampInt(row + m_scrollTop, m_scrollTop, m_scrollBottom);
    } else {
        m_cursor.row = clampInt(row, 0, m_rows - 1);
    }
    m_cursor.col = clampInt(col, 0, m_cols - 1);
    notifyChanged(before);
}

int TerminalScreen::clampRelativeRow(int dRow) const
{
    const int target = m_cursor.row + dRow;
    if (dRow < 0) {
        // Moving up: stop at the top margin unless we started above it (DEC STD 070 / xterm).
        const int top = (m_cursor.row < m_scrollTop) ? 0 : m_scrollTop;
        return std::max(target, top);
    }
    if (dRow > 0) {
        // Moving down: stop at the bottom margin unless we started below it.
        const int bottom = (m_cursor.row > m_scrollBottom) ? (m_rows - 1) : m_scrollBottom;
        return std::min(target, bottom);
    }
    return m_cursor.row;
}

void TerminalScreen::moveCursorBy(int dRow, int dCol)
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    m_cursor.row = clampRelativeRow(dRow);
    m_cursor.col = clampInt(m_cursor.col + dCol, 0, m_cols - 1);
    notifyChanged(before);
}

void TerminalScreen::setCursorRow(int row)
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    if (m_originMode) {
        m_cursor.row = clampInt(row + m_scrollTop, m_scrollTop, m_scrollBottom);
    } else {
        m_cursor.row = clampInt(row, 0, m_rows - 1);
    }
    notifyChanged(before);
}

void TerminalScreen::setCursorColumn(int col)
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    m_cursor.col = clampInt(col, 0, m_cols - 1);
    notifyChanged(before);
}

void TerminalScreen::cursorNextLine(int n)
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    m_cursor.row = clampRelativeRow(std::max(1, n));
    m_cursor.col = 0;
    notifyChanged(before);
}

void TerminalScreen::cursorPreviousLine(int n)
{
    const Cursor before = m_cursor;
    m_pendingWrap = false;
    m_cursor.row = clampRelativeRow(-std::max(1, n));
    m_cursor.col = 0;
    notifyChanged(before);
}

TerminalScreen::SavedCursor& TerminalScreen::activeSavedCursor()
{
    return m_alternate ? m_savedCursorAlt : m_savedCursor;
}

void TerminalScreen::saveCursor()
{
    SavedCursor& s = activeSavedCursor();
    s.cursor = m_cursor;
    s.attr = m_attr;
    s.originMode = m_originMode;
    s.autoWrap = m_autoWrap;
    s.valid = true;
}

void TerminalScreen::restoreCursor()
{
    const Cursor before = m_cursor;
    const SavedCursor& s = activeSavedCursor();
    m_pendingWrap = false;
    if (s.valid) {
        m_cursor.row = s.cursor.row;
        m_cursor.col = s.cursor.col;
        m_attr = s.attr;
        m_originMode = s.originMode;
        m_autoWrap = s.autoWrap;
        clampCursor();
    } else {
        // Nothing saved: xterm homes the cursor and resets the attributes.
        m_cursor.row = 0;
        m_cursor.col = 0;
        m_attr = Attributes();
    }
    notifyChanged(before);
}

void TerminalScreen::setCursorVisible(bool visible)
{
    if (m_cursor.visible == visible) {
        return;
    }
    m_cursor.visible = visible;
    markDirty(m_cursor.row);
    notifyChanged(m_cursor);
}

// ---- Erase / edit -------------------------------------------------------------------------

void TerminalScreen::eraseCells(int row, int from, int to)
{
    Line& line = m_screen[row];
    from = clampInt(from, 0, m_cols);
    to = clampInt(to, 0, m_cols);
    if (from >= to) {
        return;
    }
    // Never leave half of a wide character behind.
    if (from > 0 && line.cells[from].isWideTrail()) {
        --from;
    }
    if (to < m_cols && line.cells[to - 1].attr.has(Terminal::WideLead)) {
        ++to;
    }
    const Cell blank = blankCell();
    for (int c = from; c < to; ++c) {
        line.cells[c] = blank;
    }
    markDirty(row);
}

void TerminalScreen::fixWideCells(Line& line) const
{
    const int len = line.length();
    const Cell blank = blankCell();
    for (int c = 0; c < len; ++c) {
        Cell& cell = line.cells[c];
        if (cell.attr.has(Terminal::WideLead)) {
            if (c + 1 >= len || !line.cells[c + 1].isWideTrail()) {
                cell = blank;
            }
        } else if (cell.isWideTrail()) {
            if (c == 0 || !line.cells[c - 1].attr.has(Terminal::WideLead)) {
                cell = blank;
            }
        }
    }
}

void TerminalScreen::eraseInDisplay(int mode)
{
    const Cursor before = m_cursor;
    switch (mode) {
    case 0:
        eraseCells(m_cursor.row, m_cursor.col, m_cols);
        m_screen[m_cursor.row].wrapped = false;
        for (int r = m_cursor.row + 1; r < m_rows; ++r) {
            m_screen[r] = blankLine();
        }
        markAllDirty();
        break;
    case 1:
        for (int r = 0; r < m_cursor.row; ++r) {
            m_screen[r] = blankLine();
        }
        eraseCells(m_cursor.row, 0, m_cursor.col + 1);
        markAllDirty();
        break;
    case 2:
        for (int r = 0; r < m_rows; ++r) {
            m_screen[r] = blankLine();
        }
        markAllDirty();
        break;
    case 3:
        if (!m_scrollback.isEmpty()) {
            m_scrollback.clear();
            m_scrollbackDirty = true;
            markAllDirty();
        }
        break;
    default:
        return;
    }
    m_pendingWrap = false;
    notifyChanged(before);
}

void TerminalScreen::eraseInLine(int mode)
{
    const Cursor before = m_cursor;
    switch (mode) {
    case 0:
        eraseCells(m_cursor.row, m_cursor.col, m_cols);
        m_screen[m_cursor.row].wrapped = false;
        break;
    case 1:
        eraseCells(m_cursor.row, 0, m_cursor.col + 1);
        break;
    case 2:
        eraseCells(m_cursor.row, 0, m_cols);
        m_screen[m_cursor.row].wrapped = false;
        break;
    default:
        return;
    }
    m_pendingWrap = false;
    notifyChanged(before);
}

void TerminalScreen::insertLines(int n)
{
    if (m_cursor.row < m_scrollTop || m_cursor.row > m_scrollBottom) {
        return;
    }
    const Cursor before = m_cursor;
    n = clampInt(n, 1, m_scrollBottom - m_cursor.row + 1);
    for (int i = 0; i < n; ++i) {
        m_screen.removeAt(m_scrollBottom);
        m_screen.insert(m_cursor.row, blankLine());
    }
    m_cursor.col = 0;
    m_pendingWrap = false;
    markAllDirty();
    notifyChanged(before);
}

void TerminalScreen::deleteLines(int n)
{
    if (m_cursor.row < m_scrollTop || m_cursor.row > m_scrollBottom) {
        return;
    }
    const Cursor before = m_cursor;
    n = clampInt(n, 1, m_scrollBottom - m_cursor.row + 1);
    for (int i = 0; i < n; ++i) {
        m_screen.removeAt(m_cursor.row);
        m_screen.insert(m_scrollBottom, blankLine());
    }
    m_cursor.col = 0;
    m_pendingWrap = false;
    markAllDirty();
    notifyChanged(before);
}

void TerminalScreen::insertChars(int n)
{
    const Cursor before = m_cursor;
    Line& line = m_screen[m_cursor.row];
    const int col = m_cursor.col;
    n = clampInt(n, 1, m_cols - col);
    if (line.cells[col].isWideTrail()) {
        eraseCells(m_cursor.row, col, col + 1);
    }
    for (int c = m_cols - 1; c >= col + n; --c) {
        line.cells[c] = line.cells[c - n];
    }
    const Cell blank = blankCell();
    for (int c = col; c < col + n; ++c) {
        line.cells[c] = blank;
    }
    fixWideCells(line);
    m_pendingWrap = false;
    markDirty(m_cursor.row);
    notifyChanged(before);
}

void TerminalScreen::deleteChars(int n)
{
    const Cursor before = m_cursor;
    Line& line = m_screen[m_cursor.row];
    const int col = m_cursor.col;
    n = clampInt(n, 1, m_cols - col);
    if (line.cells[col].isWideTrail()) {
        eraseCells(m_cursor.row, col, col + 1);
    }
    for (int c = col; c + n < m_cols; ++c) {
        line.cells[c] = line.cells[c + n];
    }
    const Cell blank = blankCell();
    for (int c = std::max(col, m_cols - n); c < m_cols; ++c) {
        line.cells[c] = blank;
    }
    fixWideCells(line);
    m_pendingWrap = false;
    markDirty(m_cursor.row);
    notifyChanged(before);
}

void TerminalScreen::eraseChars(int n)
{
    const Cursor before = m_cursor;
    n = clampInt(n, 1, m_cols - m_cursor.col);
    eraseCells(m_cursor.row, m_cursor.col, m_cursor.col + n);
    m_pendingWrap = false;
    notifyChanged(before);
}

void TerminalScreen::scrollRegionUp(int n, bool pushToScrollback)
{
    if (n <= 0) {
        return;
    }
    n = std::min(n, m_scrollBottom - m_scrollTop + 1);
    for (int i = 0; i < n; ++i) {
        Line top = m_screen.takeAt(m_scrollTop);
        if (pushToScrollback && m_scrollbackMax > 0) {
            m_scrollback.append(std::move(top));
            m_scrollbackDirty = true;
        }
        m_screen.insert(m_scrollBottom, blankLine());
    }
    if (pushToScrollback) {
        trimScrollback();
    }
    markAllDirty();
}

void TerminalScreen::scrollRegionDown(int n)
{
    if (n <= 0) {
        return;
    }
    n = std::min(n, m_scrollBottom - m_scrollTop + 1);
    for (int i = 0; i < n; ++i) {
        m_screen.removeAt(m_scrollBottom);
        m_screen.insert(m_scrollTop, blankLine());
    }
    markAllDirty();
}

void TerminalScreen::scrollUp(int n)
{
    if (n <= 0) {
        return;
    }
    scrollRegionUp(n, false);
    notifyChanged(m_cursor);
}

void TerminalScreen::scrollDown(int n)
{
    if (n <= 0) {
        return;
    }
    scrollRegionDown(n);
    notifyChanged(m_cursor);
}

void TerminalScreen::repeatLastChar(int n)
{
    if (m_lastChar == 0 || n <= 0) {
        return;
    }
    const Cursor before = m_cursor;
    n = std::min(n, m_rows * m_cols);
    for (int i = 0; i < n; ++i) {
        doPutChar(m_lastChar);
    }
    notifyChanged(before);
}

// ---- Modes --------------------------------------------------------------------------------

void TerminalScreen::setScrollRegion(int top, int bottom)
{
    top = clampInt(top, 0, m_rows - 1);
    bottom = clampInt(bottom, 0, m_rows - 1);
    if (top >= bottom) {
        return;
    }
    const Cursor before = m_cursor;
    m_scrollTop = top;
    m_scrollBottom = bottom;
    m_pendingWrap = false;
    m_cursor.row = m_originMode ? m_scrollTop : 0;
    m_cursor.col = 0;
    notifyChanged(before);
}

void TerminalScreen::setAutoWrap(bool on)
{
    m_autoWrap = on;
    if (!on) {
        m_pendingWrap = false;
    }
}

void TerminalScreen::setOriginMode(bool on)
{
    const Cursor before = m_cursor;
    m_originMode = on;
    m_pendingWrap = false;
    m_cursor.row = on ? m_scrollTop : 0;
    m_cursor.col = 0;
    notifyChanged(before);
}

void TerminalScreen::setInsertMode(bool on)
{
    m_insertMode = on;
}

void TerminalScreen::setAlternateScreen(bool on, bool saveCursor)
{
    if (on == m_alternate) {
        return;
    }
    const Cursor before = m_cursor;
    if (on) {
        if (saveCursor) {
            this->saveCursor(); // primary slot: the alternate screen is not active yet
        }
        m_savedScreen = std::move(m_screen);
        m_screen.clear();
        m_screen.reserve(m_rows);
        for (int r = 0; r < m_rows; ++r) {
            m_screen.append(blankLine());
        }
        m_alternate = true;
    } else {
        m_screen = std::move(m_savedScreen);
        m_savedScreen.clear();
        while (m_screen.size() > m_rows) {
            m_screen.removeLast();
        }
        while (m_screen.size() < m_rows) {
            m_screen.append(blankLine());
        }
        for (Line& l : m_screen) {
            fitLineToColumns(l, m_cols);
        }
        m_alternate = false;
        if (saveCursor) {
            const SavedCursor& s = m_savedCursor;
            if (s.valid) {
                m_cursor.row = s.cursor.row;
                m_cursor.col = s.cursor.col;
                m_attr = s.attr;
                m_originMode = s.originMode;
                m_autoWrap = s.autoWrap;
            }
        }
    }
    m_pendingWrap = false;
    clampCursor();
    markAllDirty();
    notifyChanged(before);
}

void TerminalScreen::setTitle(const QString& title)
{
    if (m_title == title) {
        return;
    }
    m_title = title;
    emit titleChanged(m_title);
}

// ---- Tabs ---------------------------------------------------------------------------------

void TerminalScreen::setTabStop()
{
    if (m_cursor.col >= 0 && m_cursor.col < m_tabStops.size()) {
        m_tabStops.setBit(m_cursor.col);
    }
}

void TerminalScreen::clearTabStop()
{
    if (m_cursor.col >= 0 && m_cursor.col < m_tabStops.size()) {
        m_tabStops.clearBit(m_cursor.col);
    }
}

void TerminalScreen::clearAllTabStops()
{
    m_tabStops.fill(false);
}

void TerminalScreen::resetTabStops()
{
    m_tabStops.fill(false);
    for (int c = 8; c < m_cols; c += 8) {
        m_tabStops.setBit(c);
    }
}

// ---- Whole-screen -------------------------------------------------------------------------

void TerminalScreen::reset()
{
    const Cursor before = m_cursor;
    m_attr = Attributes();
    m_alternate = false;
    m_savedScreen.clear();
    m_screen.clear();
    m_screen.reserve(m_rows);
    for (int r = 0; r < m_rows; ++r) {
        m_screen.append(blankLine());
    }
    if (!m_scrollback.isEmpty()) {
        m_scrollback.clear();
        m_scrollbackDirty = true;
    }
    m_cursor = Cursor();
    m_savedCursor = SavedCursor();
    m_savedCursorAlt = SavedCursor();
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_autoWrap = true;
    m_originMode = false;
    m_insertMode = false;
    m_pendingWrap = false;
    m_lastChar = 0;
    resetTabStops();
    markAllDirty();
    notifyChanged(before);
}

void TerminalScreen::clearScreen()
{
    const Cursor before = m_cursor;
    for (int r = 0; r < m_rows; ++r) {
        m_screen[r] = blankLine();
    }
    m_cursor.row = m_originMode ? m_scrollTop : 0;
    m_cursor.col = 0;
    m_pendingWrap = false;
    markAllDirty();
    notifyChanged(before);
}

void TerminalScreen::clearScrollback()
{
    if (m_scrollback.isEmpty()) {
        return;
    }
    m_scrollback.clear();
    m_scrollbackDirty = true;
    markAllDirty();
    notifyChanged(m_cursor);
}

void TerminalScreen::clearAll()
{
    const Cursor before = m_cursor;
    for (int r = 0; r < m_rows; ++r) {
        m_screen[r] = blankLine();
    }
    if (m_alternate) {
        // The primary grid comes back verbatim on ?1049l / ?47l: blank it too, with default
        // attributes (the current background belongs to the alternate-screen program).
        Cell plain;
        plain.ch = U' ';
        for (Line& l : m_savedScreen) {
            l.cells.fill(plain, l.cells.size());
            l.wrapped = false;
        }
    }
    if (!m_scrollback.isEmpty()) {
        m_scrollback.clear();
        m_scrollbackDirty = true;
    }
    m_cursor.row = m_originMode ? m_scrollTop : 0;
    m_cursor.col = 0;
    m_pendingWrap = false;
    markAllDirty();
    notifyChanged(before);
}

void TerminalScreen::pushScreenToScrollback()
{
    const Cursor before = m_cursor;
    if (!m_alternate && m_scrollbackMax > 0) {
        int last = m_rows - 1;
        while (last >= 0 && isBlankLine(m_screen[last])) {
            --last;
        }
        last = std::max(last, m_cursor.row);
        for (int r = 0; r <= last; ++r) {
            m_scrollback.append(m_screen[r]);
        }
        m_scrollback.last().wrapped = false;
        trimScrollback();
        m_scrollbackDirty = true;
    }
    for (int r = 0; r < m_rows; ++r) {
        m_screen[r] = blankLine();
    }
    m_cursor.row = m_originMode ? m_scrollTop : 0;
    m_cursor.col = 0;
    m_pendingWrap = false;
    markAllDirty();
    notifyChanged(before);
}

// ---- Helpers ------------------------------------------------------------------------------

Line TerminalScreen::blankLine() const
{
    Line l;
    l.cells.fill(blankCell(), m_cols);
    return l;
}

Cell TerminalScreen::blankCell() const
{
    Cell c;
    c.ch = U' ';
    c.attr.bg = m_attr.bg;
    return c;
}

void TerminalScreen::clampCursor()
{
    m_cursor.row = clampInt(m_cursor.row, 0, m_rows - 1);
    m_cursor.col = clampInt(m_cursor.col, 0, m_cols - 1);
}

void TerminalScreen::fitLineToColumns(Line& line, int cols) const
{
    const int len = line.length();
    if (len > cols) {
        line.cells.resize(cols);
        if (cols > 0 && line.cells[cols - 1].attr.has(Terminal::WideLead)) {
            line.cells[cols - 1] = blankCell();
        }
    } else if (len < cols) {
        const Cell blank = blankCell();
        line.cells.reserve(cols);
        for (int c = len; c < cols; ++c) {
            line.cells.append(blank);
        }
    }
}

void TerminalScreen::trimScrollback()
{
    const qsizetype excess = m_scrollback.size() - m_scrollbackMax;
    if (excess > 0) {
        m_scrollback.remove(0, excess);
        m_scrollbackDropped += excess;
    }
}
