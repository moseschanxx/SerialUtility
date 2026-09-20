#include "ui/HexDumpView.h"

#include <QFont>
#include <QPalette>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>
#include <QTime>

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
    trimToMaxLines();
}

void HexDumpView::appendReceived(const QByteArray& bytes)
{
    appendChunk(bytes, false);
}

void HexDumpView::appendSent(const QByteArray& bytes)
{
    appendChunk(bytes, true);
}

void HexDumpView::clearAll()
{
    clear();
}

void HexDumpView::appendChunk(const QByteArray& bytes, bool tx)
{
    if (bytes.isEmpty()) {
        return;
    }
    QScrollBar* bar = verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum();

    QString header;
    if (m_showTimestamps) {
        header += QLatin1Char('[');
        header += QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
        header += QStringLiteral("] ");
    }
    header += tx ? QStringLiteral("TX ") : QStringLiteral("RX ");
    header += tr("%n bytes", nullptr, static_cast<int>(bytes.size()));

    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    if (!document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(header, m_headerFormat);
    cursor.insertBlock();
    // hexDump() joins its lines with '\n', which insertText turns into blocks.
    cursor.insertText(HexUtils::hexDump(bytes, 0, m_bytesPerLine), tx ? m_txFormat : m_rxFormat);
    cursor.endEditBlock();

    trimToMaxLines();

    if (wasAtBottom) {
        bar->setValue(bar->maximum());
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
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, excess);
    cursor.removeSelectedText();
    // QPlainTextEdit remembers the first visible line as a block *number* (see
    // QPlainTextEditPrivate::append(), which does topBlock-- for the same reason). Removing blocks
    // in front of it would otherwise show a later block, so re-anchor on the same content.
    // The view is NoWrap, so one block == one scrollbar line.
    bar->setValue(qMax(0, oldValue - excess));
}
