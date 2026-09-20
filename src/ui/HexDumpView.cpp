#include "ui/HexDumpView.h"

#include <QFont>
#include <QPalette>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTime>

#include <utility>

#include "core/HexUtils.h"

namespace {

constexpr int kDefaultFontPointSize = 10;
const QColor kBackground(0x1E, 0x1E, 0x1E);   ///< matches the "dark" terminal theme
const QColor kForeground(0xD4, 0xD4, 0xD4);
const QColor kHeaderColor(0x80, 0x80, 0x80);
const QColor kRxColor(0x4E, 0xC9, 0xB0);
const QColor kTxColor(0xCE, 0x91, 0x78);

/// Snap to one of the supported widths (8, 16, 32).
int normalizeBytesPerLine(int n)
{
    if (n <= 8) {
        return 8;
    }
    if (n <= 16) {
        return 16;
    }
    return 32;
}

} // namespace

HexDumpView::HexDumpView(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setWordWrapMode(QTextOption::NoWrap);
    setCenterOnScroll(false);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);

    QFont mono(QStringLiteral("Consolas"), kDefaultFontPointSize);
    mono.setStyleHint(QFont::Monospace);
    mono.setFixedPitch(true);
    setFont(mono);

    QPalette pal = palette();
    pal.setColor(QPalette::Base, kBackground);
    pal.setColor(QPalette::Window, kBackground);
    pal.setColor(QPalette::Text, kForeground);
    setPalette(pal);
    viewport()->setAutoFillBackground(true);

    m_headerFormat.setForeground(kHeaderColor);
    m_rxFormat.setForeground(kRxColor);
    m_txFormat.setForeground(kTxColor);
}

int HexDumpView::bytesPerLine() const
{
    return m_bytesPerLine;
}

void HexDumpView::setBytesPerLine(int n)
{
    m_bytesPerLine = normalizeBytesPerLine(n);
}

bool HexDumpView::showTimestamps() const
{
    return m_showTimestamps;
}

void HexDumpView::setShowTimestamps(bool on)
{
    m_showTimestamps = on;
}

int HexDumpView::maxLines() const
{
    return m_maxLines;
}

void HexDumpView::setMaxLines(int lines)
{
    m_maxLines = qMax(1, lines);
    dropUnreachablePending();
    trimToMaxLines();
}

void HexDumpView::appendReceived(const QByteArray& bytes)
{
    append(bytes, false);
}

void HexDumpView::appendSent(const QByteArray& bytes)
{
    append(bytes, true);
}

void HexDumpView::clearAll()
{
    m_pending.clear();
    m_pendingLines = 0;
    clear();
}

void HexDumpView::flushPending()
{
    if (m_pending.isEmpty()) {
        return;
    }
    QScrollBar* bar = verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum();

    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    for (const PendingChunk& chunk : std::as_const(m_pending)) {
        renderChunk(cursor, chunk);
    }
    cursor.endEditBlock();
    m_pending.clear();
    m_pendingLines = 0;

    trimToMaxLines();

    if (wasAtBottom) {
        bar->setValue(bar->maximum());
    }
}

void HexDumpView::showEvent(QShowEvent* event)
{
    // Before the first paint, and before the base class adjusts the scrollbars to the content.
    flushPending();
    QPlainTextEdit::showEvent(event);
}

void HexDumpView::append(const QByteArray& bytes, bool tx)
{
    if (bytes.isEmpty()) {
        return;
    }
    // Everything the rendering depends on is captured now, so a chunk rendered later (hidden
    // view) comes out exactly as it would have on arrival.
    PendingChunk chunk;
    if (m_showTimestamps) {
        chunk.header += QLatin1Char('[');
        chunk.header += QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
        chunk.header += QStringLiteral("] ");
    }
    chunk.header += tx ? QStringLiteral("TX ") : QStringLiteral("RX ");
    chunk.header += tr("%n bytes", nullptr, static_cast<int>(bytes.size()));
    chunk.bytes = bytes;
    chunk.bytesPerLine = m_bytesPerLine;
    chunk.tx = tx;
    chunk.lines = 1 + static_cast<int>((bytes.size() + m_bytesPerLine - 1) / m_bytesPerLine);

    m_pendingLines += chunk.lines;
    m_pending.append(std::move(chunk));
    dropUnreachablePending();
    if (isVisible()) {
        flushPending();   // shown: render on arrival, as always
    }
}

void HexDumpView::renderChunk(QTextCursor& cursor, const PendingChunk& chunk)
{
    if (!document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(chunk.header, m_headerFormat);
    cursor.insertBlock();
    // hexDump() joins its lines with '\n', which insertText turns into blocks.
    cursor.insertText(HexUtils::hexDump(chunk.bytes, 0, chunk.bytesPerLine), chunk.tx ? m_txFormat : m_rxFormat);
}

void HexDumpView::dropUnreachablePending()
{
    // The oldest queued chunk can go while the rest still fills maxLines() blocks on its own:
    // trimToMaxLines() would remove every block it rendered anyway. The newest chunk always stays,
    // however large (its tail survives the trim, like an oversized chunk rendered on arrival).
    while (m_pending.size() > 1 && m_pendingLines - m_pending.constFirst().lines >= m_maxLines) {
        m_pendingLines -= m_pending.constFirst().lines;
        m_pending.removeFirst();
    }
}

void HexDumpView::trimToMaxLines()
{
    const int excess = document()->blockCount() - m_maxLines;
    if (excess <= 0) {
        return;
    }
    QScrollBar* bar = verticalScrollBar();
    const int oldValue = bar->value();
    // Select everything before the first block that stays. findBlockByNumber() is a tree lookup;
    // stepping NextBlock `excess` times would lay out (shape) every block on the way.
    const QTextBlock keep = document()->findBlockByNumber(excess);
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.setPosition(keep.position(), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    // QPlainTextEdit remembers the first visible line as a block *number* (see
    // QPlainTextEditPrivate::append(), which does topBlock-- for the same reason). Removing blocks
    // in front of it would otherwise show a later block, so re-anchor on the same content.
    // The view is NoWrap, so one block == one scrollbar line.
    bar->setValue(qMax(0, oldValue - excess));
}
