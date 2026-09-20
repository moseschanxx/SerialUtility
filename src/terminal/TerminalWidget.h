#pragma once

#include <QAbstractScrollArea>
#include <QFont>
#include <QTimer>
#include <QPoint>
#include <QByteArray>
#include <QStringEncoder>
#include <QElapsedTimer>
#include <memory>

#include "terminal/TerminalTypes.h"
#include "core/LineEnding.h"

class TerminalScreen;
class AnsiParser;

/**
 * The interactive terminal view: renders a TerminalScreen (with scrollback) and turns
 * keyboard / mouse / IME input into bytes to transmit.
 *
 * Data flow
 *   serial RX  -> feedData()  -> AnsiParser -> TerminalScreen -> (coalesced repaint)
 *   keyboard   -> keyPressEvent -> sendData(bytes)            -> SessionWidget -> serial TX
 *
 * Rendering
 *  - Monospace font; cell size = horizontalAdvance("M") x lineSpacing (wide chars = 2 cells).
 *  - Paints only the rows the screen marked dirty (or everything after a scroll/resize);
 *    repaints are coalesced with a 16 ms single-shot timer so 1.5 Mbaud boot logs do not
 *    starve the event loop.
 *  - Vertical scrollbar: range 0..scrollbackSize(), value = first visible absolute line.
 *    While the view is at the bottom it follows new output; scrolling up freezes it
 *    (shows a small "N new lines" hint is optional). Any key press jumps back to the bottom.
 *  - Cursor: block when focused (blinking if enabled), hollow when unfocused, hidden when
 *    DECTCEM off. Selection uses palette.selection.
 *  - Bell: flashes the background briefly (visual) and calls QApplication::beep() when enabled.
 *  - Attributes: bold (bright + bold font), dim (blend fg 50% into bg), italic, underline,
 *    strike, inverse, hidden, blink (rendered as normal - no timer).
 *
 * Input mapping (bytes sent; see docs/TERMINAL_EMULATION.md "Key mapping")
 *  - Enter/Return -> LineEnding::bytes(enterSends())   (default CR)
 *  - Backspace -> 0x7F or 0x08 (backspaceSendsDelete()); Shift+Backspace sends the other one
 *  - Tab -> 0x09 (the widget must keep Tab: override focusNextPrevChild / event())
 *  - Esc -> 0x1B; Delete -> ESC[3~; Insert -> ESC[2~; Home -> ESC[H; End -> ESC[F
 *  - Arrows -> ESC[A/B/C/D, or ESC O A/B/C/D in DECCKM application mode
 *  - PgUp/PgDn -> ESC[5~ / ESC[6~ ; Shift+PgUp/PgDn scroll the view instead
 *  - F1-F4 -> ESC O P/Q/R/S ; F5..F12 -> ESC[15~ 17~ 18~ 19~ 20~ 21~ 23~ 24~
 *  - Ctrl+A..Z -> 0x01..0x1A ; Ctrl+[ 0x1B ; Ctrl+\ 0x1C ; Ctrl+] 0x1D ; Ctrl+Space 0x00
 *  - Alt+<key> -> ESC + key bytes
 *  - Ctrl+Shift+C / Ctrl+Insert -> copySelection() ; Ctrl+Shift+V / Shift+Insert -> paste()
 *  - Ctrl+C with an active selection -> copy (and clear selection); without -> 0x03
 *  - Ctrl+wheel / Ctrl+'+' / Ctrl+'-' / Ctrl+0 -> zoom (font size) ; emits fontZoomed()
 *  - Text (incl. IME commit) -> encoded with the current encoding (QStringEncoder)
 *  - Middle click -> paste selection/clipboard ; right click -> context menu
 *    (Copy, Paste, Select All, Clear Scrollback, Reset Terminal, Sync Terminal Size, Find...)
 *  - Paste: newlines are converted to the Enter bytes; in bracketed paste mode wrapped in
 *    ESC[200~ ... ESC[201~. Large pastes are sent in one write (pacing is the port's job).
 *  - Drag & drop: a dropped file emits fileDropped(path); dropped text is pasted.
 *
 * Selection: click-drag (cell granularity), double-click selects a word
 * (TerminalScreen::wordBoundsAt), triple-click selects the line; Shift+click extends.
 * Selection anchors are absolute line coordinates so they survive scrollback growth.
 *
 * Optional (nice to have if time permits): incremental find in scrollback via
 * findNext()/findPrevious() with highlighted matches.
 */
class TerminalWidget : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    TerminalScreen* screen() const;
    AnsiParser* parser() const;

    // ---- Appearance -----------------------------------------------------------------
    void setTerminalFont(const QFont& font);   ///< recomputes cell metrics and rows/cols
    QFont terminalFont() const;
    void setColorPalette(const Terminal::Palette& palette);
    Terminal::Palette colorPalette() const;
    void setScrollbackMax(int lines);
    void setCursorBlink(bool on);
    void setBellEnabled(bool on);

    // ---- Behaviour ------------------------------------------------------------------
    void setEnterSends(LineEnding::Mode mode);
    LineEnding::Mode enterSends() const;
    void setBackspaceSendsDelete(bool on);
    bool backspaceSendsDelete() const;
    void setLocalEcho(bool on);              ///< typed bytes are also fed to the screen
    bool localEcho() const;
    bool setEncoding(const QString& name);   ///< both decoder (parser) and encoder
    QString encoding() const;
    void setImplicitCr(bool on);
    /// When true (disconnected), key presses are swallowed (no sendData) except
    /// navigation/copy shortcuts; the cursor is drawn hollow.
    void setInputEnabled(bool on);
    bool inputEnabled() const;

    // ---- State ----------------------------------------------------------------------
    int columns() const;
    int visibleRows() const;
    bool hasSelection() const;
    QString selectedText() const;
    bool isAtBottom() const;                 ///< view follows output

public slots:
    /// Feed bytes received from the device. Safe to call at high rates.
    void feedData(const QByteArray& data);
    void clearScreen();        ///< keeps scrollback (pushes screen into it)
    void clearScrollback();
    void resetTerminal();      ///< RIS + parser reset
    void copySelection();
    void paste();
    void pasteText(const QString& text);
    void selectAll();
    void clearSelection();
    void scrollToBottom();
    void scrollLines(int delta);
    void scrollPages(int delta);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    bool findNext(const QString& needle, bool caseSensitive = false);
    bool findPrevious(const QString& needle, bool caseSensitive = false);

signals:
    /// Bytes to transmit to the device (keyboard, paste, DSR replies).
    void sendData(const QByteArray& data);
    /// Emitted when the grid size changes (font, resize); SessionWidget may show it in the status bar.
    void gridSizeChanged(int rows, int cols);
    void titleChanged(const QString& title);
    void bellRang();
    void selectionChanged();
    void cursorPositionChanged(int row, int col);
    void fileDropped(const QString& path);
    void fontZoomed(const QFont& font);
    /// Context-menu request for actions the widget does not own (e.g. Sync Terminal Size).
    void syncSizeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool event(QEvent* event) override;
    bool focusNextPrevChild(bool next) override;   ///< return false so Tab reaches keyPressEvent

private slots:
    void onScreenContentChanged();
    void onScreenSizeChanged(int rows, int cols);
    void onScrollbackChanged(int size);
    void onBell();
    void onBlinkTimeout();
    void scheduleRepaint();
    void performRepaint();

private:
    struct Selection
    {
        bool active = false;
        int anchorLine = 0, anchorCol = 0;   ///< absolute coordinates where the drag started
        int endLine = 0, endCol = 0;
        void normalized(int& l0, int& c0, int& l1, int& c1) const;   ///< ordered start/end
        bool isEmpty() const { return !active || (anchorLine == endLine && anchorCol == endCol); }
    };

    void updateCellMetrics();
    void updateGridSize();                    ///< rows/cols from viewport size -> screen()->resize()
    void updateScrollBar();
    int firstVisibleLine() const;             ///< absolute index of the top row of the viewport
    QPoint cellAt(const QPoint& viewportPos) const;   ///< (absolute line, col), clamped
    void paintLine(QPainter& painter, const Terminal::Line& line, int absoluteIndex, int y);
    void paintCursor(QPainter& painter);
    bool isSelected(int absoluteLine, int col) const;
    QByteArray keyToBytes(QKeyEvent* event, bool& handled) const;
    QByteArray encode(const QString& text) const;
    void transmit(const QByteArray& bytes);   ///< emits sendData + local echo

    // ---- Implementation helpers (private; added by the term-widget package) ------------
    void applyFont();                         ///< metrics + grid + repaint after m_font changed
    void setFontPointSize(int pointSize);     ///< clamped zoom step; emits fontZoomed() on change
    void updateViewportPalette();             ///< viewport QPalette follows m_palette (no flicker)
    void updateBlinkTimer();                  ///< start/stop blinking from focus / input / blink flags
    void restartBlink();                      ///< show the cursor and restart the blink phase (on input)
    QColor currentBackground() const;         ///< palette background, or the bell-flash colour
    void paintRun(QPainter& painter, const Terminal::Line& line, int from, int to, int y, bool selected);
    QRect cursorRect() const;                 ///< viewport rect of the cursor cell (meaningful when at bottom)
    void setSelectionRange(int anchorLine, int anchorCol, int endLine, int endCol);   ///< inclusive cells
    void selectWordAt(int absoluteLine, int col);
    void selectLineAt(int absoluteLine);
    void publishSelection();                  ///< X11 PRIMARY selection when the platform supports it
    void ensureLineVisible(int absoluteLine);
    bool handleLocalShortcut(QKeyEvent* event);   ///< copy/paste/zoom/scroll keys that never reach the device

    TerminalScreen* m_screen;
    AnsiParser* m_parser;
    Terminal::Palette m_palette;
    QFont m_font;
    int m_baseFontPointSize = 10;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_cellAscent = 12;
    LineEnding::Mode m_enterSends = LineEnding::Mode::CR;
    bool m_backspaceSendsDelete = true;
    bool m_localEcho = false;
    bool m_inputEnabled = false;
    bool m_cursorBlink = true;
    bool m_cursorBlinkState = true;
    bool m_bellEnabled = true;
    bool m_bellFlash = false;
    QString m_encoding = QStringLiteral("UTF-8");
    std::unique_ptr<QStringEncoder> m_encoder;
    Selection m_selection;
    int m_clickCount = 0;
    QTimer m_repaintTimer;
    QTimer m_blinkTimer;
    QTimer m_bellTimer;
    bool m_repaintPending = false;
    bool m_followOutput = true;
    QString m_lastFind;

    // ---- Implementation state (private; added by the term-widget package) --------------
    QFont m_renderFonts[4];                   ///< regular, bold, italic, bold+italic; letter-spaced to the cell
    int m_underlinePos = 1;                   ///< px below the baseline
    int m_strikePos = 4;                      ///< px above the baseline
    QElapsedTimer m_clickTimer;               ///< multi-click detection (double -> word, triple -> line)
    QPoint m_lastClickCell;
    bool m_selecting = false;                 ///< left button held: drag extends the selection
    int m_wheelAccumulator = 0;               ///< sub-notch wheel deltas (touchpads)
};
