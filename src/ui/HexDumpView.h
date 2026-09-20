#pragma once

#include <QByteArray>
#include <QList>
#include <QPlainTextEdit>
#include <QString>
#include <QTextCharFormat>

class QTextCursor;

/**
 * Read-only hex dump of a session's traffic, shown instead of the terminal when the
 * user toggles "Hex View" (SessionWidget::ViewMode::HexDump).
 *
 * Each appended chunk produces:
 *   [HH:mm:ss.zzz] RX 12 bytes
 *   00000000  48 65 6C 6C 6F ...   |Hello...|
 * RX headers/bytes in green-ish, TX in orange (colours from the widget palette-independent
 * constants so it matches the dark terminal theme). Offsets restart at 0 for every chunk.
 * The document is capped at maxLines() blocks (oldest removed) so it cannot grow unbounded.
 * Monospace font follows AppSettings::terminalFont().
 *
 * High-rate output (DESIGN.md 4.7): while the view is shown every chunk is rendered on arrival.
 * While it is hidden (the terminal page is in front, or the tab is not the current one) the
 * chunk is only queued - a QTextDocument insertion plus trim per 4 KB chunk costs more than the
 * whole terminal pipeline - and the queue keeps just the newest chunks that maxLines() can show;
 * older ones would be trimmed right after being rendered. The header text (timestamp, direction,
 * byte count) and bytesPerLine() are captured when the chunk arrives, so the document that
 * showEvent() (or flushPending()) renders is identical to the one immediate rendering would have
 * produced. Only document()/toPlainText() of a hidden view lag behind until flushPending().
 */
class HexDumpView : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit HexDumpView(QWidget* parent = nullptr);

    int bytesPerLine() const;
    void setBytesPerLine(int n);        ///< 8, 16 or 32
    bool showTimestamps() const;
    void setShowTimestamps(bool on);
    int maxLines() const;
    void setMaxLines(int lines);        ///< default 5000

public slots:
    void appendReceived(const QByteArray& data);
    void appendSent(const QByteArray& data);
    void clearAll();
    /// Render the chunks queued while the view was hidden into the document now (see the class
    /// comment). A no-op while nothing is queued; showEvent() calls it before the first paint.
    /// Public slot added at integration for callers that read document() of a hidden view.
    void flushPending();

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// One appended chunk, formatted as it arrived (header text, direction, hex width).
    struct PendingChunk
    {
        QString header;
        QByteArray bytes;
        int bytesPerLine = 16;
        bool tx = false;
        int lines = 0;   ///< blocks this chunk renders: header + one per bytesPerLine bytes
    };

    void append(const QByteArray& data, bool tx);
    void renderChunk(QTextCursor& cursor, const PendingChunk& chunk);   ///< at the document end
    void dropUnreachablePending();   ///< oldest queued chunks that maxLines() could never show
    void trimToMaxLines();

    int m_bytesPerLine = 16;
    bool m_showTimestamps = true;
    int m_maxLines = 5000;
    QTextCharFormat m_rxFormat;
    QTextCharFormat m_txFormat;
    QTextCharFormat m_headerFormat;
    QList<PendingChunk> m_pending;   ///< chunks appended while hidden, oldest first
    int m_pendingLines = 0;          ///< sum of PendingChunk::lines
};
