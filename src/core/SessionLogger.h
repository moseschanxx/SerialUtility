#pragma once

#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <memory>

/**
 * Writes a session's traffic to a file.
 *
 * Formats:
 *  - Raw:  received bytes written verbatim (TX never included) - replayable capture.
 *  - Text: received bytes written as-is but with a "[yyyy-MM-dd HH:mm:ss.zzz] " prefix at
 *          the start of every new line (a line starts after '\n'); if includeTx, sent
 *          bytes are written as a separate line "[timestamp] TX> <escaped bytes>".
 *          Non-UTF-8 sequences are passed through unchanged.
 *  - HexDump: every chunk becomes "[timestamp] RX/TX (N bytes)\n" + HexUtils::hexDump(chunk) + "\n".
 *
 * File is opened in append mode; for Text/HexDump formats a header line
 * "# BuildAI Serial Utility log - <port> <settings> - started <time>" is written on start, preceded
 * by '\n' when the existing file does not already end in one, so the header always begins a new
 * line (Raw captures stay a verbatim byte stream). Data is flushed after every write (logs must
 * survive a crash of the board or the app); a failed write or flush stops the log and emits error().
 */
class SessionLogger : public QObject
{
    Q_OBJECT
public:
    enum class Format { Raw, Text, HexDump };
    Q_ENUM(Format)

    explicit SessionLogger(QObject* parent = nullptr);
    ~SessionLogger() override;

    /// Start logging to `filePath` (directories are created). `header` is written for
    /// Text/HexDump formats. Returns false and emits error() when the file cannot be opened or
    /// the header cannot be written; started() is emitted only when true is returned.
    bool start(const QString& filePath, Format format, bool includeTx, const QString& header = QString());
    void stop();
    bool isActive() const;
    QString filePath() const;
    Format format() const;
    qint64 bytesWritten() const;

    /// "<dir>/<port>_<yyyy-MM-dd_HH-mm-ss>.log" (":" and "/" in port names replaced by "_").
    static QString suggestFileName(const QString& portName, const QString& directory);
    /// "raw" | "text" | "hex"  <->  Format (unknown -> Text)
    static QString formatToString(Format format);
    static Format formatFromString(const QString& key);

public slots:
    void logReceived(const QByteArray& data);
    void logSent(const QByteArray& data);

signals:
    void started(const QString& filePath);
    void stopped(const QString& filePath, qint64 bytesWritten);
    void error(const QString& message);

private:
    void writeRaw(const QByteArray& bytes);
    static QString timestamp();

    std::unique_ptr<QFile> m_file;
    Format m_format = Format::Text;
    bool m_includeTx = true;
    bool m_atLineStart = true;   ///< Text format: next RX byte begins a new line
    qint64 m_bytesWritten = 0;
};
