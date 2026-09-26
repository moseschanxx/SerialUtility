#include "terminal/TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStringConverter>
#include <QUrl>
#include <QVarLengthArray>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <limits>
#include <utility>

#include "app/Logging.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalScreen.h"

namespace {

constexpr int kRepaintIntervalMs = 16;    ///< coalescing window for screen updates
constexpr int kBlinkIntervalMs = 530;
constexpr int kBellFlashMs = 120;
constexpr int kBellSuppressMs = 250;      ///< xterm-style bell suppression: at most one beep/flash per window
constexpr int kDragScrollIntervalMs = 50; ///< edge auto-scroll while dragging a selection: one line per tick
constexpr int kMinFontPointSize = 6;
constexpr int kMaxFontPointSize = 40;
constexpr int kDefaultFontPointSize = 10;
constexpr int kWheelUnitsPerLine = 40;    ///< 120 units per notch -> 3 lines
constexpr int kDefaultRows = 24;
constexpr int kDefaultCols = 80;
constexpr int kDefaultScrollback = 10000;
constexpr int kMinGrid = 2;
constexpr int kPauseBadgeIntervalMs = 100;   ///< badge repaint coalescing while pending bytes grow
constexpr int kPauseBadgeMargin = 6;         ///< px from the top / right edge of the viewport
constexpr int kPauseBadgePadX = 10;
constexpr int kPauseBadgePadY = 4;
constexpr int kPauseBadgeRadius = 6;

/// ESC followed by `tail` (avoids "\x1b" hex-escape pitfalls with following hex digits).
QByteArray esc(const char* tail)
{
    return QByteArray("\x1b") + tail;
}

/// Linear RGB blend: t = 0 -> a, t = 1 -> b.
QColor blend(const QColor& a, const QColor& b, qreal t)
{
    const qreal u = 1.0 - t;
    return QColor(qRound(a.red() * u + b.red() * t), qRound(a.green() * u + b.green() * t),
                  qRound(a.blue() * u + b.blue() * t));
}

/// The built-in "dark" colours (see TerminalTheme.h); SessionWidget pushes the configured
/// theme through setColorPalette() right after construction.
Terminal::Palette defaultPalette()
{
    Terminal::Palette p;
    p.name = QStringLiteral("dark");
    const char* const ansi[16] = {"#000000", "#CD3131", "#0DBC79", "#E5E510", "#2472C8", "#BC3FBC",
                                  "#11A8CD", "#E5E5E5", "#666666", "#F14C4C", "#23D18B", "#F5F543",
                                  "#3B8EEA", "#D670D6", "#29B8DB", "#FFFFFF"};
    for (int i = 0; i < 16; ++i) {
        p.ansi[i] = QColor(QLatin1StringView(ansi[i]));
    }
    p.foreground = QColor(0xD4, 0xD4, 0xD4);
    p.background = QColor(0x1E, 0x1E, 0x1E);
    p.cursor = QColor(0xD4, 0xD4, 0xD4);
    p.cursorText = QColor(0x1E, 0x1E, 0x1E);
    p.selection = QColor(0x26, 0x4F, 0x78);
    p.selectionText = QColor(0xFF, 0xFF, 0xFF);
    return p;
}

/// xterm modifier parameter: 1 + Shift(1) + Alt(2) + Ctrl(4); 1 means "no modifier".
int xtermModifierParam(Qt::KeyboardModifiers mods)
{
    int value = 1;
    if (mods.testFlag(Qt::ShiftModifier)) {
        value += 1;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        value += 2;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        value += 4;
    }
    return value;
}

/// Keys that belong to the main window's QActions even while the terminal has focus.
bool isPassThroughShortcut(int key, Qt::KeyboardModifiers mods)
{
    if (mods.testFlag(Qt::MetaModifier)) {
        return true;
    }
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);
    if (!ctrl && !alt) {
        return key == Qt::Key_F2 || key == Qt::Key_F3 || key == Qt::Key_F5;   // Connect / Disconnect / Refresh
    }
    if (ctrl && !alt) {
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
            return true;   // Ctrl+Tab / Ctrl+Shift+Tab: next / previous tab
        }
        if (!shift && (key == Qt::Key_T || key == Qt::Key_W || key == Qt::Key_Comma)) {
            return true;   // New session / Close session / Preferences
        }
        if (shift && key >= Qt::Key_A && key <= Qt::Key_Z) {
            // Ctrl+Shift+<letter>: main-window actions (Copy, Paste, Hex View, Find, Send File,
            // Clear, Replay Log, Quit). When no action claims the key it comes back to
            // keyPressEvent() and is sent as the Ctrl+<letter> control byte.
            return true;
        }
    }
    return false;
}

/// Widget-local shortcuts that never produce bytes for the device.
enum class LocalAction {
    None,
    Copy,
    CopyIfSelection,   ///< Ctrl+C: copy only when a selection exists, otherwise 0x03
    Paste,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    ScrollPageUp,
    ScrollPageDown
};

LocalAction localActionFor(const QKeyEvent* event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);
    if (alt || mods.testFlag(Qt::MetaModifier)) {
        return LocalAction::None;
    }
    if (ctrl && shift && key == Qt::Key_C) {
        return LocalAction::Copy;
    }
    if (ctrl && !shift && key == Qt::Key_Insert) {
        return LocalAction::Copy;
    }
    if (ctrl && shift && key == Qt::Key_V) {
        return LocalAction::Paste;
    }
    if (shift && !ctrl && key == Qt::Key_Insert) {
        return LocalAction::Paste;
    }
    if (ctrl && !shift && key == Qt::Key_C) {
        return LocalAction::CopyIfSelection;
    }
    if (ctrl && (key == Qt::Key_Plus || key == Qt::Key_Equal)) {
        return LocalAction::ZoomIn;
    }
    if (ctrl && key == Qt::Key_Minus) {
        return LocalAction::ZoomOut;
    }
    if (ctrl && key == Qt::Key_0) {
        return LocalAction::ZoomReset;
    }
    if (shift && !ctrl && key == Qt::Key_PageUp) {
        return LocalAction::ScrollPageUp;
    }
    if (shift && !ctrl && key == Qt::Key_PageDown) {
        return LocalAction::ScrollPageDown;
    }
    return LocalAction::None;
}

/// True when the key press should reach keyPressEvent() instead of the application's
/// shortcut map (QEvent::ShortcutOverride decision).
bool wantsKeyAsInput(const QKeyEvent* event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (isPassThroughShortcut(key, mods)) {
        return false;
    }
    switch (key) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Insert:
    case Qt::Key_Delete:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_Right:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_F1:
    case Qt::Key_F2:
    case Qt::Key_F3:
    case Qt::Key_F4:
    case Qt::Key_F5:
    case Qt::Key_F6:
    case Qt::Key_F7:
    case Qt::Key_F8:
    case Qt::Key_F9:
    case Qt::Key_F10:
    case Qt::Key_F11:
    case Qt::Key_F12:
        return true;
    default:
        break;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            return true;
        }
        switch (key) {
        case Qt::Key_Space:
        case Qt::Key_At:
        case Qt::Key_BracketLeft:
        case Qt::Key_Backslash:
        case Qt::Key_BracketRight:
        case Qt::Key_AsciiCircum:
        case Qt::Key_Underscore:
        case Qt::Key_Question:
        case Qt::Key_Plus:
        case Qt::Key_Equal:
        case Qt::Key_Minus:
        case Qt::Key_0:
            return true;
        default:
            // AltGr combinations are reported as Ctrl+Alt on Windows and carry text.
            return mods.testFlag(Qt::AltModifier) && !event->text().isEmpty();
        }
    }
    return !event->text().isEmpty();   // plain / Shift / Alt + printable
}

/// Selection boundaries covering the cells from `anchorCell` to `cell` inclusive, in either
/// order. Points are (absolute line, column); the resulting end column is exclusive and the
/// anchor cell stays selected when dragging backwards.
void selectionBounds(const QPoint& anchorCell, const QPoint& cell, int& anchorLine, int& anchorCol, int& endLine,
                     int& endCol)
{
    const bool backward = cell.x() < anchorCell.x() || (cell.x() == anchorCell.x() && cell.y() < anchorCell.y());
    anchorLine = anchorCell.x();
    endLine = cell.x();
    if (backward) {
        anchorCol = anchorCell.y() + 1;
        endCol = cell.y();
    } else {
        anchorCol = anchorCell.y();
        endCol = cell.y() + 1;
    }
}

/// Text of a line built from its cells (wide trails skipped) with, for every UTF-16 unit,
/// the cell column it came from. Used by find so that match positions map back to cells.
void buildSearchText(const Terminal::Line& line, QString& text, QVarLengthArray<int, 512>& columnOf)
{
    const int count = static_cast<int>(line.cells.size());
    text.reserve(count);
    for (int col = 0; col < count; ++col) {
        const Terminal::Cell& cell = line.cells.at(col);
        if (cell.attr.has(Terminal::WideTrail)) {
            continue;
        }
        const char32_t ch = (cell.ch == 0) ? U' ' : cell.ch;
        if (QChar::requiresSurrogates(ch)) {
            text.append(QChar(QChar::highSurrogate(ch)));
            text.append(QChar(QChar::lowSurrogate(ch)));
            columnOf.append(col);
            columnOf.append(col);
        } else {
            text.append(QChar(static_cast<char16_t>(ch)));
            columnOf.append(col);
        }
    }
}

/// Search one line for `needle`. Forward: first match starting at a column >= fromCol.
/// Backward: last match starting at a column <= fromCol. Returns the match as
/// [matchCol, matchEndCol) in cell columns.
bool findInLine(const Terminal::Line& line, const QString& needle, Qt::CaseSensitivity cs, int fromCol, bool backward,
                int& matchCol, int& matchEndCol)
{
    QString text;
    QVarLengthArray<int, 512> columnOf;
    buildSearchText(line, text, columnOf);
    if (text.isEmpty()) {
        return false;
    }
    qsizetype index = -1;
    if (!backward) {
        qsizetype start = 0;
        while (start < text.size() && columnOf[start] < fromCol) {
            ++start;
        }
        if (start >= text.size()) {
            return false;
        }
        index = text.indexOf(needle, start, cs);
    } else {
        qsizetype start = text.size() - 1;
        while (start >= 0 && columnOf[start] > fromCol) {
            --start;
        }
        if (start < 0) {
            return false;
        }
        index = text.lastIndexOf(needle, start, cs);
    }
    if (index < 0) {
        return false;
    }
    const qsizetype last = index + needle.size() - 1;
    matchCol = columnOf[index];
    const int lastCol = columnOf[last];
    const bool lastWide = line.cells.at(lastCol).attr.has(Terminal::WideLead);
    matchEndCol = lastCol + (lastWide ? 2 : 1);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------------------

void TerminalWidget::Selection::normalized(int& l0, int& c0, int& l1, int& c1) const
{
    // anchor/end are cell boundaries (a column index is the boundary before that cell), so
    // anchor == end is an empty selection and the ordered end column is exclusive (textRange()).
    if (anchorLine < endLine || (anchorLine == endLine && anchorCol <= endCol)) {
        l0 = anchorLine;
        c0 = anchorCol;
        l1 = endLine;
        c1 = endCol;
    } else {
        l0 = endLine;
        c0 = endCol;
        l1 = anchorLine;
        c1 = anchorCol;
    }
}

// ---------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------

TerminalWidget::TerminalWidget(QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_screen(new TerminalScreen(kDefaultRows, kDefaultCols, kDefaultScrollback, this))
    , m_parser(new AnsiParser(m_screen, this))
    , m_palette(defaultPalette())
    , m_font(QStringLiteral("Consolas"), kDefaultFontPointSize)
{
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_baseFontPointSize = kDefaultFontPointSize;
    m_encoder = std::make_unique<QStringEncoder>(QStringConverter::Utf8);

    setFrameStyle(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setInputMethodHints(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    setAcceptDrops(true);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAutoFillBackground(false);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setFocusProxy(this);
    updateViewportPalette();

    m_repaintTimer.setSingleShot(true);
    connect(&m_repaintTimer, &QTimer::timeout, this, &TerminalWidget::performRepaint);
    m_blinkTimer.setInterval(kBlinkIntervalMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, &TerminalWidget::onBlinkTimeout);
    m_bellTimer.setSingleShot(true);
    connect(&m_bellTimer, &QTimer::timeout, this, [this]() {
        m_bellFlash = false;
        viewport()->update();
    });
    m_dragScrollTimer.setInterval(kDragScrollIntervalMs);
    connect(&m_dragScrollTimer, &QTimer::timeout, this, &TerminalWidget::onDragScrollTimeout);
    m_pauseBadgeTimer.setSingleShot(true);
    m_pauseBadgeTimer.setInterval(kPauseBadgeIntervalMs);
    connect(&m_pauseBadgeTimer, &QTimer::timeout, this, &TerminalWidget::onPauseBadgeTimeout);

    connect(m_screen, &TerminalScreen::contentChanged, this, &TerminalWidget::onScreenContentChanged);
    connect(m_screen, &TerminalScreen::sizeChanged, this, &TerminalWidget::onScreenSizeChanged);
    connect(m_screen, &TerminalScreen::scrollbackChanged, this, &TerminalWidget::onScrollbackChanged);
    connect(m_screen, &TerminalScreen::bellRequested, this, &TerminalWidget::onBell);
    connect(m_screen, &TerminalScreen::titleChanged, this, &TerminalWidget::titleChanged);
    connect(m_screen, &TerminalScreen::cursorMoved, this, &TerminalWidget::cursorPositionChanged);
    connect(m_screen, &TerminalScreen::cursorMoved, this, &TerminalWidget::scheduleRepaint);

    // DSR / DA replies: transmitted even without local echo, but only while input is enabled.
    connect(m_parser, &AnsiParser::responseRequested, this, [this](const QByteArray& reply) {
        if (m_inputEnabled && !reply.isEmpty()) {
            emit sendData(reply);
        }
    });

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        m_followOutput = value >= verticalScrollBar()->maximum();
        scheduleFullRepaint();
    });

    updateCellMetrics();
    updateScrollBar();
}

TerminalWidget::~TerminalWidget()
{
    m_repaintTimer.stop();
    m_blinkTimer.stop();
    m_bellTimer.stop();
    m_dragScrollTimer.stop();
    m_pauseBadgeTimer.stop();
    // The parser references the screen: make sure it goes first.
    delete m_parser;
    m_parser = nullptr;
}

TerminalScreen* TerminalWidget::screen() const
{
    return m_screen;
}

AnsiParser* TerminalWidget::parser() const
{
    return m_parser;
}

// ---------------------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------------------

void TerminalWidget::setTerminalFont(const QFont& font)
{
    QFont f = font;
    if (f.pointSize() <= 0) {
        const qreal pt = f.pointSizeF();
        f.setPointSize(pt > 0 ? qMax(1, qRound(pt)) : kDefaultFontPointSize);
    }
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    m_font = f;
    m_baseFontPointSize = f.pointSize();
    qCDebug(lcUi) << "terminal font" << f.family() << f.pointSize();
    applyFont();
}

QFont TerminalWidget::terminalFont() const
{
    return m_font;
}

void TerminalWidget::setColorPalette(const Terminal::Palette& palette)
{
    m_palette = palette;
    updateViewportPalette();
    viewport()->update();
}

Terminal::Palette TerminalWidget::colorPalette() const
{
    return m_palette;
}

void TerminalWidget::setScrollbackMax(int lines)
{
    m_screen->setScrollbackMax(qMax(0, lines));
    updateScrollBar();
    viewport()->update();
}

void TerminalWidget::setCursorBlink(bool on)
{
    m_cursorBlink = on;
    updateBlinkTimer();
    viewport()->update();
}

void TerminalWidget::setBellEnabled(bool on)
{
    m_bellEnabled = on;
}

// ---------------------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------------------

void TerminalWidget::setEnterSends(LineEnding::Mode mode)
{
    m_enterSends = mode;
}

LineEnding::Mode TerminalWidget::enterSends() const
{
    return m_enterSends;
}

void TerminalWidget::setBackspaceSendsDelete(bool on)
{
    m_backspaceSendsDelete = on;
}

bool TerminalWidget::backspaceSendsDelete() const
{
    return m_backspaceSendsDelete;
}

void TerminalWidget::setLocalEcho(bool on)
{
    m_localEcho = on;
}

bool TerminalWidget::localEcho() const
{
    return m_localEcho;
}

bool TerminalWidget::setEncoding(const QString& name)
{
    const QString trimmed = name.trimmed();
    std::unique_ptr<QStringEncoder> encoder;
    if (trimmed.compare(QStringLiteral("System"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(QStringLiteral("Locale"), Qt::CaseInsensitive) == 0) {
        encoder = std::make_unique<QStringEncoder>(QStringConverter::System);
    } else if (!trimmed.isEmpty()) {
        encoder = std::make_unique<QStringEncoder>(QAnyStringView(trimmed));
    }
    const bool ok = encoder && encoder->isValid();
    if (ok) {
        m_encoder = std::move(encoder);
        m_encoding = trimmed;
    } else {
        qCWarning(lcUi) << "unsupported encoding" << name << "- falling back to UTF-8";
        m_encoder = std::make_unique<QStringEncoder>(QStringConverter::Utf8);
        m_encoding = QStringLiteral("UTF-8");
    }
    m_parser->setEncoding(m_encoding);
    return ok;
}

QString TerminalWidget::encoding() const
{
    return m_encoding;
}

void TerminalWidget::setImplicitCr(bool on)
{
    m_parser->setImplicitCr(on);
}

void TerminalWidget::setInputEnabled(bool on)
{
    if (m_inputEnabled == on) {
        return;
    }
    m_inputEnabled = on;
    updateBlinkTimer();
    viewport()->update();
}

bool TerminalWidget::inputEnabled() const
{
    return m_inputEnabled;
}

void TerminalWidget::setPauseWhileSelecting(bool on)
{
    if (m_pauseWhileSelecting == on) {
        return;
    }
    m_pauseWhileSelecting = on;
    qCDebug(lcUi) << "pause output while selecting" << on;
    if (!on) {
        resumeOutput();   // keeps the selection; the display simply flows again
    }
    // Turning it on affects the next selection only: a selection that already exists does not
    // freeze the display retroactively.
}

bool TerminalWidget::pauseWhileSelecting() const
{
    return m_pauseWhileSelecting;
}

void TerminalWidget::setRightClickPastes(bool on)
{
    if (m_rightClickPastes == on) {
        return;
    }
    m_rightClickPastes = on;
    qCDebug(lcUi) << "right click pastes (cmd.exe style)" << on;
}

bool TerminalWidget::rightClickPastes() const
{
    return m_rightClickPastes;
}

void TerminalWidget::setPauseBufferLimit(qint64 bytes)
{
    m_pauseBufferLimit = qMax<qint64>(0, bytes);
}

qint64 TerminalWidget::pauseBufferLimit() const
{
    return m_pauseBufferLimit;
}

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------

int TerminalWidget::columns() const
{
    return m_screen->cols();
}

int TerminalWidget::visibleRows() const
{
    return m_screen->rows();
}

bool TerminalWidget::hasSelection() const
{
    return !m_selection.isEmpty();
}

QString TerminalWidget::selectedText() const
{
    if (!hasSelection()) {
        return {};
    }
    int l0 = 0;
    int c0 = 0;
    int l1 = 0;
    int c1 = 0;
    m_selection.normalized(l0, c0, l1, c1);
    return m_screen->textRange(l0, c0, l1, c1);
}

bool TerminalWidget::isAtBottom() const
{
    return verticalScrollBar()->value() >= verticalScrollBar()->maximum();
}

bool TerminalWidget::isOutputPaused() const
{
    return m_outputPaused;
}

qint64 TerminalWidget::pendingPausedBytes() const
{
    return m_pendingPaused.size();
}

// ---------------------------------------------------------------------------------------
// Public slots
// ---------------------------------------------------------------------------------------

void TerminalWidget::feedData(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    if (m_outputPaused) {
        // Mark mode: the screen (and with it the selection) must not move. Queue the bytes;
        // resumeOutput() parses them in one feed.
        const qint64 total = static_cast<qint64>(m_pendingPaused.size()) + bytes.size();
        if (total > m_pauseBufferLimit) {
            // Never lose data: the display flows again, the selection stays copyable.
            qCInfo(lcUi) << "pause buffer limit" << m_pauseBufferLimit << "exceeded with" << total
                         << "bytes pending - resuming output";
            m_pendingPaused += bytes;
            resumeOutput();
            emit pauseBufferOverflow(total);
            return;
        }
        m_pendingPaused += bytes;
        schedulePauseBadgeRepaint();
        return;
    }
    m_parser->feed(bytes);
}

void TerminalWidget::clearScreen()
{
    // While paused: drop the selection, clear, and only then let the queued output continue on
    // the cleared screen (clearSelection() alone would parse it *before* the clear).
    const bool wasPaused = m_outputPaused;
    const QByteArray deferred = takePendingOutput();
    if (wasPaused) {
        clearSelection();
    }
    m_screen->pushScreenToScrollback();
    scrollToBottom();
    scheduleRepaint();
    feedData(deferred);
}

void TerminalWidget::clearScrollback()
{
    const QByteArray deferred = takePendingOutput();
    clearSelection();
    m_screen->clearScrollback();
    updateScrollBar();
    scheduleRepaint();
    feedData(deferred);
}

void TerminalWidget::clearAll()
{
    // Toolbar "Clear": nothing of the previous output survives - neither on the screen nor in the
    // scrollback - while attributes, modes and the parser state stay (resetTerminal() is the
    // RIS). Like clearScreen(): a paused display is cleared first, the queued bytes follow.
    const QByteArray deferred = takePendingOutput();
    clearSelection();
    // Visible grid, scrollback and - while top/vi/menuconfig hold the alternate screen - the
    // primary grid saved behind it, so nothing comes back on ?1049l; cursor home, attributes kept.
    m_screen->clearAll();
    updateScrollBar();
    scrollToBottom();
    scheduleRepaint();
    feedData(deferred);
}

void TerminalWidget::resetTerminal()
{
    const QByteArray deferred = takePendingOutput();
    clearSelection();
    m_parser->reset();
    m_screen->reset();
    updateScrollBar();
    scrollToBottom();
    scheduleRepaint();
    feedData(deferred);
}

void TerminalWidget::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty()) {
        QClipboard* clipboard = QApplication::clipboard();
        clipboard->setText(text, QClipboard::Clipboard);
        if (clipboard->supportsSelection()) {
            clipboard->setText(text, QClipboard::Selection);
        }
    }
    if (m_outputPaused) {
        clearSelection();   // mark mode: copying finishes the selection and resumes the display
    }
}

void TerminalWidget::paste()
{
    pasteText(QApplication::clipboard()->text(QClipboard::Clipboard));
}

void TerminalWidget::pasteText(const QString& text)
{
    if (text.isEmpty() || !m_inputEnabled || m_outputPaused) {
        return;   // mark mode swallows pastes like every other input
    }
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QByteArray eol = LineEnding::bytes(m_enterSends);
    if (eol.isEmpty()) {
        eol = QByteArrayLiteral("\r");
    }

    QByteArray payload;
    const QList<QStringView> parts = QStringView(normalized).split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            payload += eol;
        }
        if (!parts.at(i).isEmpty()) {
            payload += encode(parts.at(i).toString());
        }
    }
    if (m_parser->bracketedPasteMode()) {
        payload.prepend(esc("[200~"));
        payload.append(esc("[201~"));
    }
    scrollToBottom();
    transmit(payload);
}

void TerminalWidget::selectAll()
{
    const int total = m_screen->totalLines();
    if (total <= 0) {
        return;
    }
    // Stop at the last non-blank line: blank rows below the output would otherwise each add
    // a '\n' to the copied text (and an Enter each when pasted back into the device).
    int last = total - 1;
    while (last > 0 && m_screen->absoluteLine(last).text().isEmpty()) {
        --last;
    }
    if (last == 0 && m_screen->absoluteLine(0).text().isEmpty()) {
        clearSelection();   // nothing to select
        return;
    }
    setSelectionRange(0, 0, last, qMax(0, m_screen->cols() - 1));
}

void TerminalWidget::clearSelection()
{
    m_dragScrollTimer.stop();
    if (m_selection.active) {
        m_selection = Selection();
        m_selecting = false;
        emit selectionChanged();
        viewport()->update();
    }
    resumeOutput();   // no selection, nothing to protect: the queued output flows again
}

void TerminalWidget::resumeOutput()
{
    if (!m_outputPaused) {
        return;
    }
    const QByteArray pending = takePendingOutput();
    if (!pending.isEmpty()) {
        m_parser->feed(pending);   // one feed: contentChanged / scrollbar updates fire once, repaint is coalesced
    }
}

void TerminalWidget::scrollToBottom()
{
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->maximum());
    m_followOutput = true;
}

void TerminalWidget::scrollLines(int delta)
{
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->value() + delta);
}

void TerminalWidget::scrollPages(int delta)
{
    scrollLines(delta * qMax(1, m_screen->rows()));
}

void TerminalWidget::zoomIn()
{
    setFontPointSize(m_font.pointSize() + 1);
}

void TerminalWidget::zoomOut()
{
    setFontPointSize(m_font.pointSize() - 1);
}

void TerminalWidget::resetZoom()
{
    setFontPointSize(m_baseFontPointSize);
}

bool TerminalWidget::findNext(const QString& needle, bool caseSensitive)
{
    if (needle.isEmpty()) {
        return false;
    }
    m_lastFind = needle;
    const int total = m_screen->totalLines();
    if (total <= 0) {
        return false;
    }
    int startLine = 0;
    int startCol = 0;
    if (hasSelection()) {
        int l1 = 0;
        int c1 = 0;
        m_selection.normalized(startLine, startCol, l1, c1);
        startCol += 1;   // continue after the current match
    } else {
        const Terminal::Cursor cur = m_screen->cursor();
        startLine = m_screen->scrollbackSize() + cur.row;
        startCol = cur.col;
    }
    startLine = qBound(0, startLine, total - 1);
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for (int i = 0; i <= total; ++i) {   // one extra pass wraps around to the start line
        const int lineIndex = (startLine + i) % total;
        const int fromCol = (i == 0) ? startCol : 0;
        int matchCol = 0;
        int matchEnd = 0;
        if (findInLine(m_screen->absoluteLine(lineIndex), needle, cs, fromCol, false, matchCol, matchEnd)) {
            setSelectionRange(lineIndex, matchCol, lineIndex, qMax(matchCol, matchEnd - 1));
            ensureLineVisible(lineIndex);
            return true;
        }
    }
    return false;
}

bool TerminalWidget::findPrevious(const QString& needle, bool caseSensitive)
{
    if (needle.isEmpty()) {
        return false;
    }
    m_lastFind = needle;
    const int total = m_screen->totalLines();
    if (total <= 0) {
        return false;
    }
    constexpr int wholeLine = std::numeric_limits<int>::max();
    int startLine = 0;
    int startCol = 0;
    if (hasSelection()) {
        int l1 = 0;
        int c1 = 0;
        m_selection.normalized(startLine, startCol, l1, c1);
        startCol -= 1;   // continue before the current match
    } else {
        const Terminal::Cursor cur = m_screen->cursor();
        startLine = m_screen->scrollbackSize() + cur.row;
        startCol = cur.col - 1;
    }
    if (startCol < 0) {
        startLine -= 1;
        startCol = wholeLine;
    }
    startLine = ((startLine % total) + total) % total;
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for (int i = 0; i <= total; ++i) {
        const int lineIndex = (((startLine - i) % total) + total) % total;
        const int fromCol = (i == 0) ? startCol : wholeLine;
        int matchCol = 0;
        int matchEnd = 0;
        if (findInLine(m_screen->absoluteLine(lineIndex), needle, cs, fromCol, true, matchCol, matchEnd)) {
            setSelectionRange(lineIndex, matchCol, lineIndex, qMax(matchCol, matchEnd - 1));
            ensureLineVisible(lineIndex);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------

void TerminalWidget::paintEvent(QPaintEvent* event)
{
    QElapsedTimer paintTimer;
    paintTimer.start();
    m_paintPosted = false;

    QPainter painter(viewport());
    const QRect clip = event->rect();
    painter.fillRect(clip, currentBackground());
    painter.setFont(m_renderFonts[0]);

    const int first = firstVisibleLine();
    const int total = m_screen->totalLines();
    const int rows = m_screen->rows();
    const int rowFrom = qMax(0, clip.top() / m_cellHeight);
    const int rowTo = qMin(rows - 1, clip.bottom() / m_cellHeight);
    for (int row = rowFrom; row <= rowTo; ++row) {
        const int absolute = first + row;
        if (absolute < 0 || absolute >= total) {
            break;
        }
        paintLine(painter, m_screen->absoluteLine(absolute), absolute, row * m_cellHeight);
    }
    paintCursor(painter);
    if (m_outputPaused) {
        paintPauseBadge(painter);   // last, over the cells and the cursor; never part of the model
    }
    // The screen's dirty state is consumed by performRepaint() when it posts the update, never
    // here: mutations that land between the two set fresh bits for the next coalesced pass.

    // scheduleRepaint() sizes the coalescing window from these (see the class comment).
    m_lastPaintMs = paintTimer.elapsed();
    m_lastPaintEnd.start();
}

void TerminalWidget::paintLine(QPainter& painter, const Terminal::Line& line, int absoluteIndex, int y)
{
    const int count = qMin(m_screen->cols(), static_cast<int>(line.cells.size()));
    int col = 0;
    while (col < count) {
        const Terminal::Cell& first = line.cells.at(col);
        const bool selected = isSelected(absoluteIndex, col);
        int end = col + 1;
        if (!first.attr.has(Terminal::WideLead)) {
            while (end < count && line.cells.at(end).attr == first.attr &&
                   isSelected(absoluteIndex, end) == selected) {
                ++end;
            }
        }
        paintRun(painter, line, col, end, y, selected);
        col = end;
    }
}

void TerminalWidget::paintRun(QPainter& painter, const Terminal::Line& line, int from, int to, int y, bool selected)
{
    const Terminal::Attributes& attr = line.cells.at(from).attr;
    if (attr.has(Terminal::WideTrail)) {
        return;   // the lead cell painted both halves
    }
    const bool wide = attr.has(Terminal::WideLead);
    const bool bold = attr.has(Terminal::Bold);
    const int width = wide ? 2 * m_cellWidth : (to - from) * m_cellWidth;
    const QRect rect(from * m_cellWidth, y, width, m_cellHeight);

    QColor fg = m_palette.resolve(attr.fg, true, bold);
    QColor bg = m_palette.resolve(attr.bg, false, false);
    if (attr.has(Terminal::Inverse)) {
        std::swap(fg, bg);
    }
    if (bg != currentBackground()) {
        painter.fillRect(rect, bg);
    }
    if (selected) {
        painter.fillRect(rect, m_palette.selection);
        if (m_palette.selectionText.isValid()) {
            fg = m_palette.selectionText;
        }
        bg = m_palette.selection.alpha() < 255 ? blend(bg, m_palette.selection, m_palette.selection.alphaF())
                                                : m_palette.selection;
    }
    if (attr.has(Terminal::Hidden)) {
        return;
    }
    if (attr.has(Terminal::Dim)) {
        fg = blend(fg, bg, 0.5);
    }

    QVarLengthArray<char32_t, 256> glyphs;
    bool blank = true;
    bool ascii = true;
    for (int i = from; i < to; ++i) {
        char32_t ch = line.cells.at(i).ch;
        if (ch == 0) {
            ch = U' ';
        }
        glyphs.append(ch);
        if (ch != U' ') {
            blank = false;
        }
        if (ch >= 0x7F) {
            ascii = false;
        }
    }

    painter.setPen(fg);
    if (!blank) {
        const int fontIndex = (bold ? 1 : 0) | (attr.has(Terminal::Italic) ? 2 : 0);
        const qreal baseline = y + m_cellAscent;
        if (ascii && !wide) {
            // Plain text: pre-shaped glyph run, one cell per glyph (no itemization/shaping per run).
            if (!drawAsciiRun(painter, glyphs, fontIndex, QPointF(rect.x(), baseline))) {
                painter.setFont(m_renderFonts[fontIndex]);
                painter.drawText(QPointF(rect.x(), baseline), QString::fromUcs4(glyphs.constData(), glyphs.size()));
            }
        } else {
            // Glyphs from fallback fonts do not share the cell advance: place each one.
            painter.setFont(m_renderFonts[fontIndex]);
            qreal x = rect.x();
            for (const char32_t ch : glyphs) {
                if (ch != U' ') {
                    painter.drawText(QPointF(x, baseline), QString::fromUcs4(&ch, 1));
                }
                x += m_cellWidth;
            }
        }
    }
    if (attr.has(Terminal::Underline)) {
        const int uy = qMin(y + m_cellHeight - 1, y + m_cellAscent + m_underlinePos);
        painter.drawLine(rect.left(), uy, rect.right(), uy);
    }
    if (attr.has(Terminal::Strike)) {
        const int sy = qMax(y, y + m_cellAscent - m_strikePos);
        painter.drawLine(rect.left(), sy, rect.right(), sy);
    }
}

bool TerminalWidget::drawAsciiRun(QPainter& painter, const QVarLengthArray<char32_t, 256>& glyphs, int fontIndex,
                                  const QPointF& origin)
{
    const GlyphCache& cache = m_glyphCache[fontIndex];
    if (!cache.valid) {
        return false;
    }
    QVarLengthArray<quint32, 256> indexes;
    QVarLengthArray<QPointF, 256> positions;
    for (int i = 0; i < glyphs.size(); ++i) {
        const char32_t ch = glyphs.at(i);
        if (ch == U' ') {
            continue;   // nothing to draw
        }
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
        const quint32 index = cache.glyphs[ch - 0x20];
        if (index == 0) {
            return false;   // the font has no glyph: drawText picks a fallback font
        }
        indexes.append(index);
        positions.append(QPointF(i * m_cellWidth, 0));
    }
    if (indexes.isEmpty()) {
        return true;
    }
    QGlyphRun run;
    run.setRawFont(cache.rawFont);
    run.setRawData(indexes.constData(), positions.constData(), static_cast<int>(indexes.size()));
    painter.drawGlyphRun(origin, run);
    return true;
}

void TerminalWidget::paintCursor(QPainter& painter)
{
    if (!m_screen->cursorVisible() || !isAtBottom()) {
        return;
    }
    const Terminal::Cursor cur = m_screen->cursor();
    if (cur.row < 0 || cur.row >= m_screen->rows() || cur.col < 0 || cur.col >= m_screen->cols()) {
        return;
    }
    const QRect rect = cursorRect();
    const QColor cursorColor = m_palette.cursor.isValid() ? m_palette.cursor : m_palette.foreground;
    const bool solid = hasFocus() && m_inputEnabled;
    if (!solid) {
        painter.setPen(cursorColor);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
        return;
    }
    if (!m_cursorBlinkState) {
        return;   // blink-off phase
    }
    painter.fillRect(rect, cursorColor);
    const Terminal::Line& line = m_screen->line(cur.row);
    if (cur.col >= line.cells.size()) {
        return;
    }
    const Terminal::Cell& cell = line.cells.at(cur.col);
    if (cell.ch == 0 || cell.ch == U' ' || cell.attr.has(Terminal::Hidden) || cell.attr.has(Terminal::WideTrail)) {
        return;
    }
    const int fontIndex = (cell.attr.has(Terminal::Bold) ? 1 : 0) | (cell.attr.has(Terminal::Italic) ? 2 : 0);
    painter.setFont(m_renderFonts[fontIndex]);
    painter.setPen(m_palette.cursorText.isValid() ? m_palette.cursorText : m_palette.background);
    painter.drawText(QPointF(rect.x(), rect.y() + m_cellAscent), QString::fromUcs4(&cell.ch, 1));
}

QColor TerminalWidget::currentBackground() const
{
    return m_bellFlash ? blend(m_palette.background, m_palette.foreground, 0.15) : m_palette.background;
}

// ---------------------------------------------------------------------------------------
// Pause output while selecting
// ---------------------------------------------------------------------------------------

void TerminalWidget::pauseForSelection()
{
    if (!m_pauseWhileSelecting || m_outputPaused || m_selection.isEmpty()) {
        return;
    }
    setOutputPaused(true);
}

void TerminalWidget::setOutputPaused(bool paused)
{
    if (m_outputPaused == paused) {
        return;
    }
    m_outputPaused = paused;
    m_pauseBadgeTimer.stop();
    if (paused) {
        qCDebug(lcUi) << "output paused while selecting";
    } else {
        qCDebug(lcUi) << "output resumed";
        m_pauseBadgeRect = QRect();
    }
    viewport()->update();   // the badge appears / disappears
    emit outputPausedChanged(paused);
}

QByteArray TerminalWidget::takePendingOutput()
{
    QByteArray pending;
    pending.swap(m_pendingPaused);
    setOutputPaused(false);
    return pending;
}

QString TerminalWidget::pauseBadgeText() const
{
    const QString size = QLocale().formattedDataSize(m_pendingPaused.size(), 1, QLocale::DataSizeTraditionalFormat);
    return tr("⏸ Output paused  %1 waiting   Enter: copy  Esc: cancel").arg(size);
}

QFont TerminalWidget::pauseBadgeFont() const
{
    QFont font = m_font;
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    font.setPointSize(qMax(kMinFontPointSize, m_font.pointSize() - 1));
    return font;
}

QRect TerminalWidget::pauseBadgeRect() const
{
    const QFontMetrics fm(pauseBadgeFont(), viewport());
    const int maxTextWidth = qMax(1, viewport()->width() - 2 * kPauseBadgeMargin - 2 * kPauseBadgePadX);
    const int textWidth = qMin(maxTextWidth, fm.horizontalAdvance(pauseBadgeText()));
    const int width = textWidth + 2 * kPauseBadgePadX;
    const int height = fm.height() + 2 * kPauseBadgePadY;
    return QRect(viewport()->width() - kPauseBadgeMargin - width, kPauseBadgeMargin, width, height);
}

void TerminalWidget::paintPauseBadge(QPainter& painter)
{
    const QFont font = pauseBadgeFont();
    const QFontMetrics fm(font, viewport());
    const QRect rect = pauseBadgeRect();
    m_pauseBadgeRect = rect;
    const QString text = fm.elidedText(pauseBadgeText(), Qt::ElideRight, rect.width() - 2 * kPauseBadgePadX);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Inverse of the terminal colours so the badge reads on every theme: a light pill with dark
    // text on the dark theme and vice versa, translucent enough to hint at the text underneath.
    QColor fill = m_palette.foreground;
    fill.setAlpha(0xE0);
    QColor border = m_palette.background;
    border.setAlpha(0x80);
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), kPauseBadgeRadius, kPauseBadgeRadius);
    painter.setFont(font);
    painter.setPen(m_palette.background);
    painter.drawText(QPointF(rect.x() + kPauseBadgePadX, rect.y() + kPauseBadgePadY + fm.ascent()), text);
    painter.restore();
}

void TerminalWidget::schedulePauseBadgeRepaint()
{
    if (!m_pauseBadgeTimer.isActive()) {
        m_pauseBadgeTimer.start();   // one repaint per interval, however many chunks arrive
    }
}

void TerminalWidget::onPauseBadgeTimeout()
{
    if (!m_outputPaused) {
        return;
    }
    // The pill grows with its number: repaint the old area as well so no stale edge remains.
    viewport()->update(m_pauseBadgeRect.united(pauseBadgeRect()));
}

QRect TerminalWidget::cursorRect() const
{
    const Terminal::Cursor cur = m_screen->cursor();
    int width = m_cellWidth;
    if (cur.row >= 0 && cur.row < m_screen->rows()) {
        const Terminal::Line& line = m_screen->line(cur.row);
        if (cur.col >= 0 && cur.col < line.cells.size() && line.cells.at(cur.col).attr.has(Terminal::WideLead)) {
            width *= 2;
        }
    }
    return QRect(cur.col * m_cellWidth, cur.row * m_cellHeight, width, m_cellHeight);
}

bool TerminalWidget::isSelected(int absoluteLine, int col) const
{
    if (m_selection.isEmpty()) {
        return false;
    }
    int l0 = 0;
    int c0 = 0;
    int l1 = 0;
    int c1 = 0;
    m_selection.normalized(l0, c0, l1, c1);
    if (absoluteLine < l0 || absoluteLine > l1) {
        return false;
    }
    if (absoluteLine == l0 && col < c0) {
        return false;
    }
    if (absoluteLine == l1 && col >= c1) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Geometry / metrics / scrollbar
// ---------------------------------------------------------------------------------------

void TerminalWidget::updateCellMetrics()
{
    QFont base = m_font;
    base.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    const QFontMetricsF fm(base, viewport());
    const qreal advance = fm.horizontalAdvance(QLatin1Char('M'));
    m_cellWidth = qMax(1, qCeil(advance));
    m_cellHeight = qMax(1, qCeil(fm.lineSpacing()));
    m_cellAscent = qMax(0, qRound(fm.ascent()));
    m_underlinePos = qMax(1, qRound(fm.underlinePos()));
    m_strikePos = qMax(1, qRound(fm.strikeOutPos()));

    // Force every glyph to advance exactly one cell so runs drawn as a string stay on the grid.
    QFont render = base;
    const qreal spacing = m_cellWidth - advance;
    if (spacing > 0.01) {
        render.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    }
    for (int i = 0; i < 4; ++i) {
        m_renderFonts[i] = render;
        m_renderFonts[i].setBold((i & 1) != 0);
        m_renderFonts[i].setItalic((i & 2) != 0);
    }
    updateGlyphCache();
}

void TerminalWidget::updateGlyphCache()
{
    static const QString printable = [] {
        QString s;
        for (char16_t c = 0x20; c <= 0x7E; ++c) {
            s.append(QChar(c));
        }
        return s;
    }();
    for (int i = 0; i < 4; ++i) {
        GlyphCache& cache = m_glyphCache[i];
        cache.valid = false;
        // The raw font of the font the painter would resolve for the viewport; the letter spacing
        // is irrelevant because drawAsciiRun() places every glyph on its cell itself.
        QFont font(m_renderFonts[i], viewport());
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
        cache.rawFont = QRawFont::fromFont(font);
        if (!cache.rawFont.isValid()) {
            continue;
        }
        const QList<quint32> indexes = cache.rawFont.glyphIndexesForString(printable);
        if (indexes.size() != printable.size()) {
            continue;
        }
        for (qsizetype c = 0; c < indexes.size(); ++c) {
            cache.glyphs[c] = indexes.at(c);
        }
        cache.valid = true;
    }
}

void TerminalWidget::updateGridSize()
{
    const QSize size = viewport()->size();
    const int cols = qMax(kMinGrid, size.width() / m_cellWidth);
    const int rows = qMax(kMinGrid, size.height() / m_cellHeight);
    if (rows != m_screen->rows() || cols != m_screen->cols()) {
        m_screen->resize(rows, cols);   // -> sizeChanged -> onScreenSizeChanged
    }
}

void TerminalWidget::updateScrollBar()
{
    QScrollBar* bar = verticalScrollBar();
    const int maximum = m_screen->scrollbackSize();
    const bool follow = m_followOutput;
    bar->setRange(0, maximum);
    bar->setPageStep(qMax(1, m_screen->rows()));
    bar->setSingleStep(1);
    if (follow) {
        bar->setValue(maximum);
    }
    m_followOutput = bar->value() >= bar->maximum();
}

int TerminalWidget::firstVisibleLine() const
{
    return qBound(0, verticalScrollBar()->value(), m_screen->scrollbackSize());
}

QPoint TerminalWidget::cellAt(const QPoint& viewportPos) const
{
    const int cols = qMax(1, m_screen->cols());
    const int rows = qMax(1, m_screen->rows());
    const int col = qBound(0, viewportPos.x() / m_cellWidth, cols - 1);
    const int row = qBound(0, viewportPos.y() / m_cellHeight, rows - 1);
    const int total = qMax(1, m_screen->totalLines());
    const int line = qBound(0, firstVisibleLine() + row, total - 1);
    return QPoint(line, col);   // x = absolute line, y = column
}

void TerminalWidget::ensureLineVisible(int absoluteLine)
{
    QScrollBar* bar = verticalScrollBar();
    const int first = bar->value();
    const int rows = qMax(1, m_screen->rows());
    if (absoluteLine < first) {
        bar->setValue(qMax(0, absoluteLine));
    } else if (absoluteLine >= first + rows) {
        bar->setValue(qMin(bar->maximum(), absoluteLine - rows + 1));
    }
}

void TerminalWidget::applyFont()
{
    updateCellMetrics();
    updateGridSize();
    viewport()->update();
}

void TerminalWidget::setFontPointSize(int pointSize)
{
    const int clamped = qBound(kMinFontPointSize, pointSize, kMaxFontPointSize);
    if (clamped == m_font.pointSize()) {
        return;
    }
    m_font.setPointSize(clamped);
    applyFont();
    emit fontZoomed(m_font);
}

void TerminalWidget::updateViewportPalette()
{
    QPalette pal = viewport()->palette();
    pal.setColor(QPalette::Window, m_palette.background);
    pal.setColor(QPalette::Base, m_palette.background);
    pal.setColor(QPalette::Text, m_palette.foreground);
    viewport()->setPalette(pal);
}

void TerminalWidget::updateBlinkTimer()
{
    const bool blink = m_cursorBlink && m_inputEnabled && hasFocus();
    if (blink) {
        if (!m_blinkTimer.isActive()) {
            m_cursorBlinkState = true;
            m_blinkTimer.start();
        }
    } else {
        m_blinkTimer.stop();
        m_cursorBlinkState = true;
    }
}

void TerminalWidget::restartBlink()
{
    m_cursorBlinkState = true;
    if (m_blinkTimer.isActive()) {
        m_blinkTimer.start();   // restart the phase so the cursor stays visible while typing
    }
}

// ---------------------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------------------

void TerminalWidget::setSelectionRange(int anchorLine, int anchorCol, int endLine, int endCol)
{
    // Inclusive cells in, boundaries stored: the end boundary is the column after `endCol`.
    m_selection.active = true;
    m_selection.anchorLine = anchorLine;
    m_selection.anchorCol = anchorCol;
    m_selection.endLine = endLine;
    m_selection.endCol = endCol + 1;
    emit selectionChanged();
    publishSelection();
    viewport()->update();
    pauseForSelection();
}

void TerminalWidget::selectWordAt(int absoluteLine, int col)
{
    int from = 0;
    int to = 0;
    m_screen->wordBoundsAt(absoluteLine, col, from, to);
    if (to > from) {
        setSelectionRange(absoluteLine, from, absoluteLine, to - 1);
    } else {
        clearSelection();
    }
}

void TerminalWidget::selectLineAt(int absoluteLine)
{
    setSelectionRange(absoluteLine, 0, absoluteLine, qMax(0, m_screen->cols() - 1));
}

void TerminalWidget::publishSelection()
{
    QClipboard* clipboard = QApplication::clipboard();
    if (!clipboard->supportsSelection()) {
        return;
    }
    const QString text = selectedText();
    if (!text.isEmpty()) {
        clipboard->setText(text, QClipboard::Selection);
    }
}

// ---------------------------------------------------------------------------------------
// Input: keyboard
// ---------------------------------------------------------------------------------------

bool TerminalWidget::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        // Main-window actions (Ctrl+Shift+<letter>, Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+, F2/F3/F5...)
        // keep working while the terminal has focus; everything else the terminal wants is
        // claimed here so QAction shortcuts such as Ctrl+L or Ctrl+C cannot steal it.
        if (!isPassThroughShortcut(keyEvent->key(), keyEvent->modifiers())) {
            LocalAction action = localActionFor(keyEvent);
            if (action == LocalAction::CopyIfSelection && !hasSelection()) {
                action = LocalAction::None;
            }
            // Mark mode: Enter / Esc finish or cancel the selection even while disconnected.
            const int key = keyEvent->key();
            const bool markModeKey =
                m_outputPaused && (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape);
            if (action != LocalAction::None || markModeKey || (m_inputEnabled && wantsKeyAsInput(keyEvent))) {
                keyEvent->accept();
                return true;
            }
        }
    }
    return QAbstractScrollArea::event(event);
}

bool TerminalWidget::focusNextPrevChild(bool next)
{
    Q_UNUSED(next);
    return false;   // Tab / Shift+Tab are sent to the device
}

bool TerminalWidget::handleLocalShortcut(QKeyEvent* event)
{
    switch (localActionFor(event)) {
    case LocalAction::Copy:
        copySelection();
        return true;
    case LocalAction::CopyIfSelection:
        if (!hasSelection()) {
            return false;
        }
        copySelection();
        clearSelection();
        return true;
    case LocalAction::Paste:
        paste();
        return true;
    case LocalAction::ZoomIn:
        zoomIn();
        return true;
    case LocalAction::ZoomOut:
        zoomOut();
        return true;
    case LocalAction::ZoomReset:
        resetZoom();
        return true;
    case LocalAction::ScrollPageUp:
        scrollPages(-1);
        return true;
    case LocalAction::ScrollPageDown:
        scrollPages(1);
        return true;
    case LocalAction::None:
        break;
    }
    return false;
}

bool TerminalWidget::handlePausedKey(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        copySelection();    // copies and, while paused, clears + resumes
        clearSelection();   // nothing to copy (blank selection): still leave mark mode
        return true;
    case Qt::Key_Escape:
        clearSelection();   // cancel: no clipboard change
        return true;
    default:
        break;
    }
    switch (localActionFor(event)) {
    case LocalAction::Copy:
    case LocalAction::CopyIfSelection:
        copySelection();
        clearSelection();
        return true;
    case LocalAction::Paste:
        return true;   // swallowed: nothing may reach the device in mark mode
    case LocalAction::ZoomIn:
    case LocalAction::ZoomOut:
    case LocalAction::ZoomReset:
    case LocalAction::ScrollPageUp:
    case LocalAction::ScrollPageDown:
        return false;   // view-only shortcuts keep working (handleLocalShortcut())
    case LocalAction::None:
        break;
    }
    return true;   // every other key: swallowed, no bytes, no local echo
}

void TerminalWidget::keyPressEvent(QKeyEvent* event)
{
    // (0) mark mode: the keyboard finishes or cancels the selection, nothing reaches the device
    if (m_outputPaused && handlePausedKey(event)) {
        event->accept();
        return;
    }
    // (1) shortcuts that never reach the device
    if (handleLocalShortcut(event)) {
        event->accept();
        return;
    }
    // (2) disconnected: swallow everything else. The event must be accepted, not ignored: an
    //     ignored Tab/Backtab propagates to the parent widget, whose QWidget::event() runs the
    //     focus-chain navigation and moves the focus away (DESIGN.md 4.7: Tab never does).
    if (!m_inputEnabled) {
        event->accept();
        return;
    }
    // (3)-(6) key -> bytes
    bool handled = false;
    const QByteArray bytes = keyToBytes(event, handled);
    if (!handled) {
        event->ignore();
        return;
    }
    if (!bytes.isEmpty()) {
        scrollToBottom();
        restartBlink();
        transmit(bytes);
    }
    event->accept();
}

QByteArray TerminalWidget::keyToBytes(QKeyEvent* event, bool& handled) const
{
    handled = true;
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);
    const bool appCursor = m_parser->cursorKeyApplicationMode();
    const int modParam = xtermModifierParam(mods);

    // ESC [ 1 ; m X  with modifiers, else ESC [ X  (or ESC O X in DECCKM application mode)
    const auto cursorKey = [appCursor, modParam](char letter) {
        if (modParam > 1) {
            return esc("[1;") + QByteArray::number(modParam) + letter;
        }
        return esc(appCursor ? "O" : "[") + letter;
    };
    // ESC [ n ; m ~  with modifiers, else ESC [ n ~
    const auto tildeKey = [modParam](int code) {
        QByteArray bytes = esc("[") + QByteArray::number(code);
        if (modParam > 1) {
            bytes += ';' + QByteArray::number(modParam);
        }
        return bytes + '~';
    };
    // F1-F4: ESC O X, or ESC [ 1 ; m X with modifiers
    const auto functionKey = [modParam](char letter) {
        if (modParam > 1) {
            return esc("[1;") + QByteArray::number(modParam) + letter;
        }
        return esc("O") + letter;
    };

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (shift) {
            return QByteArrayLiteral("\n");
        }
        QByteArray eol = LineEnding::bytes(m_enterSends);
        if (eol.isEmpty()) {
            eol = QByteArrayLiteral("\r");
        }
        return alt ? esc("") + eol : eol;
    }
    case Qt::Key_Backspace: {
        bool sendDelete = m_backspaceSendsDelete;
        if (shift) {
            sendDelete = !sendDelete;
        }
        QByteArray bytes(1, sendDelete ? '\x7f' : '\x08');
        if (alt) {
            bytes.prepend('\x1b');
        }
        return bytes;
    }
    case Qt::Key_Tab:
        return QByteArrayLiteral("\t");
    case Qt::Key_Backtab:
        return esc("[Z");
    case Qt::Key_Escape:
        return alt ? QByteArrayLiteral("\x1b\x1b") : QByteArrayLiteral("\x1b");
    case Qt::Key_Insert:
        return tildeKey(2);
    case Qt::Key_Delete:
        return tildeKey(3);
    case Qt::Key_PageUp:
        return tildeKey(5);
    case Qt::Key_PageDown:
        return tildeKey(6);
    case Qt::Key_Home:
        return cursorKey('H');
    case Qt::Key_End:
        return cursorKey('F');
    case Qt::Key_Up:
        return cursorKey('A');
    case Qt::Key_Down:
        return cursorKey('B');
    case Qt::Key_Right:
        return cursorKey('C');
    case Qt::Key_Left:
        return cursorKey('D');
    case Qt::Key_F1:
        return functionKey('P');
    case Qt::Key_F2:
        return functionKey('Q');
    case Qt::Key_F3:
        return functionKey('R');
    case Qt::Key_F4:
        return functionKey('S');
    case Qt::Key_F5:
        return tildeKey(15);
    case Qt::Key_F6:
        return tildeKey(17);
    case Qt::Key_F7:
        return tildeKey(18);
    case Qt::Key_F8:
        return tildeKey(19);
    case Qt::Key_F9:
        return tildeKey(20);
    case Qt::Key_F10:
        return tildeKey(21);
    case Qt::Key_F11:
        return tildeKey(23);
    case Qt::Key_F12:
        return tildeKey(24);
    default:
        break;
    }

    QString text = event->text();
    // AltGr is reported as Ctrl+Alt on Windows; when it yields a printable character it is text.
    const bool altGr = ctrl && alt && !text.isEmpty() && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7F;

    if (ctrl && !altGr) {
        char code = 0;
        bool known = true;
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            code = static_cast<char>(key - Qt::Key_A + 1);
        } else {
            switch (key) {
            case Qt::Key_Space:
            case Qt::Key_At:
                code = '\x00';
                break;
            case Qt::Key_BracketLeft:
                code = '\x1b';
                break;
            case Qt::Key_Backslash:
                code = '\x1c';
                break;
            case Qt::Key_BracketRight:
                code = '\x1d';
                break;
            case Qt::Key_AsciiCircum:
                code = '\x1e';
                break;
            case Qt::Key_Underscore:
                code = '\x1f';
                break;
            case Qt::Key_Question:
                code = '\x7f';
                break;
            default:
                known = false;
                break;
            }
        }
        if (!known) {
            handled = false;
            return {};
        }
        QByteArray bytes(1, code);
        if (alt) {
            bytes.prepend('\x1b');
        }
        return bytes;
    }

    if (text.isEmpty() && alt && key >= 0x20 && key < 0x7F) {
        // Some platforms deliver no text for Alt+<key>: derive it from the key code.
        QChar ch(static_cast<char16_t>(key));
        if (ch.isLetter() && !shift) {
            ch = ch.toLower();
        }
        text = QString(ch);
    }
    if (text.isEmpty()) {
        handled = false;   // lone modifier or dead key
        return {};
    }
    // Drop control characters Qt may put into text() for exotic combinations.
    if (text.size() == 1 && (text.at(0).unicode() < 0x20 || text.at(0).unicode() == 0x7F)) {
        handled = false;
        return {};
    }
    QByteArray bytes = encode(text);
    if (alt && !altGr) {
        bytes.prepend('\x1b');
    }
    return bytes;
}

QByteArray TerminalWidget::encode(const QString& text) const
{
    if (text.isEmpty()) {
        return {};
    }
    if (m_encoder && m_encoder->isValid()) {
        const QByteArray encoded = (*m_encoder)(text);
        return encoded;
    }
    return text.toUtf8();
}

void TerminalWidget::transmit(const QByteArray& bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    emit sendData(bytes);
    if (m_localEcho) {
        QByteArray echo = bytes;
        echo.replace(QByteArrayLiteral("\r\n"), QByteArrayLiteral("\r"));
        echo.replace('\r', QByteArrayLiteral("\r\n"));
        feedData(echo);
    }
}

// ---------------------------------------------------------------------------------------
// Input: IME
// ---------------------------------------------------------------------------------------

void TerminalWidget::inputMethodEvent(QInputMethodEvent* event)
{
    const QString commit = event->commitString();
    if (!commit.isEmpty() && m_inputEnabled && !m_outputPaused) {
        scrollToBottom();
        restartBlink();
        transmit(encode(commit));
    }
    // Preedit text is not displayed: the device echoes what it receives.
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return cursorRect().translated(viewport()->pos());
    case Qt::ImFont:
        return m_font;
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

// ---------------------------------------------------------------------------------------
// Input: mouse
// ---------------------------------------------------------------------------------------

void TerminalWidget::mousePressEvent(QMouseEvent* event)
{
    const QPoint cell = cellAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton) {
        // Qt delivers a double click as Press, Release, Press, DblClick, Release: the
        // DblClick handler sets m_clickCount to 2, so a chained third press selects the line.
        const bool chained = m_clickCount > 0 && cell == m_lastClickCell && m_clickTimer.isValid() &&
                             m_clickTimer.elapsed() < QApplication::doubleClickInterval();
        m_clickTimer.start();
        if (chained && m_clickCount >= 2) {
            m_clickCount = 0;   // the next click starts a fresh sequence
            m_selecting = false;
            m_dragScrollTimer.stop();
            m_lastClickCell = cell;
            selectLineAt(cell.x());
        } else if (event->modifiers().testFlag(Qt::ShiftModifier) && m_selection.active) {
            // Shift+click: keep the anchor cell of the current selection, extend to `cell`.
            m_clickCount = 1;
            const bool forward =
                m_selection.anchorLine < m_selection.endLine ||
                (m_selection.anchorLine == m_selection.endLine && m_selection.anchorCol <= m_selection.endCol);
            const int anchorCell = forward ? m_selection.anchorCol : m_selection.anchorCol - 1;
            m_lastClickCell = QPoint(m_selection.anchorLine, anchorCell);
            m_selecting = true;
            selectionBounds(m_lastClickCell, cell, m_selection.anchorLine, m_selection.anchorCol, m_selection.endLine,
                            m_selection.endCol);
            emit selectionChanged();
            publishSelection();
            viewport()->update();
            pauseForSelection();
        } else {
            // Plain press: drop the old selection and start an empty one at the pressed cell;
            // it becomes visible once the pointer leaves that cell (mouseMoveEvent()).
            m_clickCount = 1;
            m_lastClickCell = cell;
            m_selecting = true;
            m_selection.active = true;
            m_selection.anchorLine = cell.x();
            m_selection.anchorCol = cell.y();
            m_selection.endLine = cell.x();
            m_selection.endCol = cell.y();
            emit selectionChanged();
            viewport()->update();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        QClipboard* clipboard = QApplication::clipboard();
        QString text;
        if (clipboard->supportsSelection()) {
            text = clipboard->text(QClipboard::Selection);
        }
        if (text.isEmpty()) {
            text = clipboard->text(QClipboard::Clipboard);
        }
        pasteText(text);
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // cmd.exe QuickEdit: a plain right click copies the selection (and, like Enter in mark
        // mode, finishes it - which resumes a paused display) or pastes the clipboard. The
        // context menu is reached with Shift (contextMenuEvent() suppresses the mouse-triggered
        // menu while the feature is on). A right press never touches a left-button selection in
        // progress: a chorded press while the left button is still held is ignored. The button
        // state of the event is authoritative: when a drag's left release never reached us (a
        // popup - e.g. the Shift+right-click menu opened mid-drag - took it) the drag is over
        // and must not keep blocking right clicks; the selection it made stays.
        const bool leftHeld = event->buttons().testFlag(Qt::LeftButton);
        if (m_selecting && !leftHeld) {
            m_selecting = false;
            m_dragScrollTimer.stop();
        }
        if (m_rightClickPastes && !event->modifiers().testFlag(Qt::ShiftModifier) && !leftHeld) {
            if (hasSelection()) {
                copySelection();    // while paused this already clears the selection and resumes
                clearSelection();   // not paused: the selection is finished as well, nothing is pasted
            } else {
                paste();            // pasteText(): CR/LF conversion, bracketed paste, dropped while disconnected
            }
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_selecting && event->buttons().testFlag(Qt::LeftButton)) {
        const QPoint pos = event->position().toPoint();
        m_dragScrollPos = pos;
        const bool outside = pos.y() < 0 || pos.y() >= viewport()->height();
        if (outside) {
            if (!m_dragScrollTimer.isActive()) {
                onDragScrollTimeout();   // scroll immediately on the crossing, then repeat
                m_dragScrollTimer.start();
            }
        } else {
            m_dragScrollTimer.stop();
            extendDragSelectionTo(pos);
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void TerminalWidget::extendDragSelectionTo(const QPoint& viewportPos)
{
    const QPoint cell = cellAt(viewportPos);   // rows outside the viewport clamp to the first/last visible row
    if (cell == m_lastClickCell && m_selection.isEmpty()) {
        return;   // jitter inside the pressed cell is not a drag yet
    }
    int anchorLine = 0;
    int anchorCol = 0;
    int endLine = 0;
    int endCol = 0;
    selectionBounds(m_lastClickCell, cell, anchorLine, anchorCol, endLine, endCol);
    if (anchorLine != m_selection.anchorLine || anchorCol != m_selection.anchorCol || endLine != m_selection.endLine ||
        endCol != m_selection.endCol) {
        m_selection.active = true;
        m_selection.anchorLine = anchorLine;
        m_selection.anchorCol = anchorCol;
        m_selection.endLine = endLine;
        m_selection.endCol = endCol;
        emit selectionChanged();
        viewport()->update();
        pauseForSelection();   // the drag left the pressed cell: the selection is real now
    }
}

void TerminalWidget::onDragScrollTimeout()
{
    if (!m_selecting) {
        m_dragScrollTimer.stop();
        return;
    }
    QScrollBar* bar = verticalScrollBar();
    if (m_dragScrollPos.y() < 0) {
        // At the top there is nothing left to scroll; the timer keeps running so that a later
        // move back below the edge is still tracked by mouseMoveEvent().
        if (bar->value() > bar->minimum()) {
            scrollLines(-1);
        }
    } else if (m_dragScrollPos.y() >= viewport()->height()) {
        if (bar->value() < bar->maximum()) {
            scrollLines(1);
        }
    } else {
        m_dragScrollTimer.stop();
        return;
    }
    extendDragSelectionTo(m_dragScrollPos);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragScrollTimer.stop();
        m_selecting = false;
        if (m_selection.active && m_selection.isEmpty()) {
            // A click without a drag: the (previous) selection is gone, and with it the pause.
            m_selection.active = false;
            emit selectionChanged();
            viewport()->update();
            resumeOutput();
        } else if (m_selection.active) {
            publishSelection();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        event->accept();   // the press did the work (paste / copy / nothing); nothing for the parent
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    const QPoint cell = cellAt(event->position().toPoint());
    m_clickCount = 2;
    m_clickTimer.start();
    m_lastClickCell = cell;
    m_selecting = false;
    m_dragScrollTimer.stop();
    selectWordAt(cell.x(), cell.y());
    event->accept();
}

void TerminalWidget::wheelEvent(QWheelEvent* event)
{
    const int dy = event->angleDelta().y();
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (dy > 0) {
            zoomIn();
        } else if (dy < 0) {
            zoomOut();
        }
        event->accept();
        return;
    }
    m_wheelAccumulator += dy;
    const int lines = m_wheelAccumulator / kWheelUnitsPerLine;
    if (lines != 0) {
        m_wheelAccumulator -= lines * kWheelUnitsPerLine;
        scrollLines(-lines);
    }
    event->accept();
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* event)
{
    // cmd.exe style: the plain right click already pasted or copied in mousePressEvent(); the
    // menu is reached with Shift+right click, the Menu key or Shift+F10 (reason Keyboard). With
    // the feature off a plain right click opens the menu as before.
    if (m_rightClickPastes && event->reason() == QContextMenuEvent::Mouse &&
        !event->modifiers().testFlag(Qt::ShiftModifier)) {
        event->accept();
        return;
    }

    QMenu menu(this);
    QAction* copyAction = menu.addAction(tr("&Copy"));
    copyAction->setEnabled(hasSelection());
    connect(copyAction, &QAction::triggered, this, &TerminalWidget::copySelection);

    QAction* pasteAction = menu.addAction(tr("&Paste"));
    pasteAction->setEnabled(m_inputEnabled && !m_outputPaused && !QApplication::clipboard()->text().isEmpty());
    connect(pasteAction, &QAction::triggered, this, &TerminalWidget::paste);

    QAction* selectAllAction = menu.addAction(tr("Select &All"));
    connect(selectAllAction, &QAction::triggered, this, &TerminalWidget::selectAll);

    menu.addSeparator();
    // Unlike the toolbar's Clear (SessionWidget::clearTerminal(): screen + scrollback + hex view)
    // this one pushes the screen into the scrollback, hence the explicit label.
    QAction* clearScreenAction = menu.addAction(tr("Clear &Screen (keep scrollback)"));
    connect(clearScreenAction, &QAction::triggered, this, &TerminalWidget::clearScreen);
    QAction* clearScrollbackAction = menu.addAction(tr("Clear Scroll&back"));
    connect(clearScrollbackAction, &QAction::triggered, this, &TerminalWidget::clearScrollback);
    QAction* resetAction = menu.addAction(tr("&Reset Terminal"));
    connect(resetAction, &QAction::triggered, this, &TerminalWidget::resetTerminal);

    menu.addSeparator();
    QAction* syncAction = menu.addAction(tr("Sync Terminal Size (stty)"));
    syncAction->setEnabled(m_inputEnabled);
    connect(syncAction, &QAction::triggered, this, &TerminalWidget::syncSizeRequested);

    menu.addSeparator();
    QAction* findAction = menu.addAction(tr("&Find..."));
    findAction->setEnabled(m_screen->totalLines() > 0);
    connect(findAction, &QAction::triggered, this, &TerminalWidget::findRequested);

    if (m_rightClickPastes) {
        // Discoverability: the menu no longer opens on a plain right click, say so at the bottom.
        menu.addSeparator();
        QAction* hintAction = menu.addAction(tr("Right click: paste / copy selection - Shift+right click: this menu"));
        hintAction->setEnabled(false);
    }

    menu.exec(event->globalPos());
    event->accept();
}

// ---------------------------------------------------------------------------------------
// Focus / resize / scrolling / drag & drop
// ---------------------------------------------------------------------------------------

void TerminalWidget::focusInEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusInEvent(event);
    updateBlinkTimer();
    viewport()->update();
}

void TerminalWidget::focusOutEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusOutEvent(event);
    updateBlinkTimer();
    viewport()->update();
}

void TerminalWidget::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateGridSize();
    viewport()->update();
}

void TerminalWidget::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    scheduleFullRepaint();   // rows are re-read from the screen model; no pixel scrolling
}

void TerminalWidget::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (mime && (mime->hasUrls() || mime->hasText())) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void TerminalWidget::dropEvent(QDropEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (!mime) {
        event->ignore();
        return;
    }
    if (mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                qCInfo(lcUi) << "file dropped on terminal" << url.toLocalFile();
                emit fileDropped(url.toLocalFile());
                event->acceptProposedAction();
                return;
            }
        }
    }
    if (mime->hasText()) {
        pasteText(mime->text());
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

// ---------------------------------------------------------------------------------------
// Private slots
// ---------------------------------------------------------------------------------------

void TerminalWidget::onScreenContentChanged()
{
    scheduleRepaint();
}

void TerminalWidget::onScreenSizeChanged(int rows, int cols)
{
    updateScrollBar();
    emit gridSizeChanged(rows, cols);
    scheduleRepaint();
}

void TerminalWidget::onScrollbackChanged(int size)
{
    Q_UNUSED(size);
    // Lines dropped from a full scrollback shift every absolute index by -1 each. Shift the
    // frozen view and the selection the same way (before the range is updated) so the same text
    // stays under the viewport and under the highlight.
    const qint64 dropped = m_screen->scrollbackDropped();
    const qint64 maxDelta = std::numeric_limits<int>::max();
    const int delta = static_cast<int>(qMin(dropped - m_seenScrollbackDropped, maxDelta));
    m_seenScrollbackDropped = dropped;
    if (delta > 0) {
        if (!m_followOutput) {
            QScrollBar* bar = verticalScrollBar();
            bar->setValue(qMax(0, bar->value() - delta));   // at 0 the view pins to the oldest surviving line
        }
        if (m_selection.active) {
            m_selection.anchorLine -= delta;
            m_selection.endLine -= delta;
            m_lastClickCell.rx() = qMax(0, m_lastClickCell.x() - delta);
            int l0 = 0;
            int c0 = 0;
            int l1 = 0;
            int c1 = 0;
            m_selection.normalized(l0, c0, l1, c1);
            if (l1 < 0) {
                clearSelection();   // the whole selection fell off the top
            } else if (l0 < 0) {
                // Clamp the earlier end (l0 < 0 <= l1, so the two ends are on different lines).
                if (m_selection.anchorLine < m_selection.endLine) {
                    m_selection.anchorLine = 0;
                    m_selection.anchorCol = 0;
                } else {
                    m_selection.endLine = 0;
                    m_selection.endCol = 0;
                }
                emit selectionChanged();
            }
        }
    }
    updateScrollBar();   // keeps the view at the bottom while following
    scheduleRepaint();
}

void TerminalWidget::onBell()
{
    emit bellRang();
    if (!m_bellEnabled) {
        return;
    }
    // Suppress bells that arrive in a burst (baud mismatch garbage, binary data): one beep and
    // one 120 ms flash per 250 ms window, so the background always returns to normal in between.
    if (m_bellSuppress.isValid() && m_bellSuppress.elapsed() < kBellSuppressMs) {
        return;
    }
    m_bellSuppress.start();
    QApplication::beep();
    m_bellFlash = true;
    m_bellTimer.start(kBellFlashMs);
    viewport()->update();
}

void TerminalWidget::onBlinkTimeout()
{
    m_cursorBlinkState = !m_cursorBlinkState;
    if (isAtBottom() && m_screen->cursorVisible()) {
        viewport()->update(cursorRect());
    }
}

void TerminalWidget::scheduleRepaint()
{
    if (m_repaintPending) {
        return;
    }
    m_repaintPending = true;
    // Coalescing window, measured from the end of the previous paint: at least
    // kRepaintIntervalMs, and at least as long as that paint took, so a paint that outlasts the
    // interval (large window, slow blit) still leaves the parser half of the wall time instead
    // of turning every chunk into a frame. After an idle period the change is painted at once.
    qint64 delay = kRepaintIntervalMs;
    if (m_lastPaintEnd.isValid()) {
        const qint64 window = qMax<qint64>(kRepaintIntervalMs, m_lastPaintMs);
        const qint64 since = m_paintPosted ? 0 : m_lastPaintEnd.elapsed();
        delay = qBound<qint64>(0, window - since, window);
    }
    m_repaintTimer.start(static_cast<int>(delay));
}

void TerminalWidget::scheduleFullRepaint()
{
    m_repaintAll = true;
    scheduleRepaint();
}

void TerminalWidget::performRepaint()
{
    m_repaintPending = false;
    // Partial repaint is only meaningful while following output: when scrolled up the visible
    // rows are scrollback lines and do not map to screen rows. A scroll (m_repaintAll) has to
    // repaint everything even when the screen itself did not change.
    const bool all = m_repaintAll || m_screen->allDirty() || !isAtBottom();
    m_repaintAll = false;
    if (!all && !m_screen->isDirty()) {
        return;   // nothing changed since the last paint (e.g. a cursorMoved coalesced into an earlier full paint)
    }
    if (all) {
        m_screen->clearDirty();
        m_paintPosted = true;
        viewport()->update();
        return;
    }
    const QBitArray dirty = m_screen->dirtyRows();
    QRegion region;
    const int width = viewport()->width();
    const int rows = qMin(static_cast<int>(dirty.size()), m_screen->rows());
    for (int row = 0; row < rows; ++row) {
        if (dirty.testBit(row)) {
            region += QRect(0, row * m_cellHeight, width, m_cellHeight);
        }
    }
    // The cursor row is always repainted (cursor may have moved onto/off it, or its visibility toggled).
    const Terminal::Cursor cur = m_screen->cursor();
    if (cur.row >= 0 && cur.row < m_screen->rows()) {
        region += QRect(0, cur.row * m_cellHeight, width, m_cellHeight);
    }
    m_screen->clearDirty();   // consume the dirty state now, before the paint event is delivered
    if (region.isEmpty()) {
        return;
    }
    m_paintPosted = true;
    viewport()->update(region);
}
