#pragma once

#include <QPlainTextEdit>
#include <QTextCharFormat>

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

private:
    void appendChunk(const QByteArray& data, bool tx);
    void trimToMaxLines();

    int m_bytesPerLine = 16;
    bool m_showTimestamps = true;
    int m_maxLines = 5000;
    QTextCharFormat m_rxFormat;
    QTextCharFormat m_txFormat;
    QTextCharFormat m_headerFormat;
};
