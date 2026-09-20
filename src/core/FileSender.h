#pragma once

#include <QObject>
#include <QString>
#include <QFile>
#include <QTimer>
#include <memory>

#include "core/LineEnding.h"

/**
 * Paces a file out over the serial link, chunk by chunk, on the GUI thread using a QTimer.
 *
 * Text mode: the file is read line by line (any of \r\n, \n, \r accepted as a line break);
 *   each line is sent with its original line ending stripped (when stripLineEndings) and
 *   `lineEnding` appended, then the sender waits lineDelayMs before the next line. Useful
 *   for pasting shell scripts / U-Boot env into a console without overrunning it.
 * Binary mode: raw bytes in chunks of chunkSize with chunkDelayMs between chunks.
 *
 * The sender does not know about the port: it emits chunkReady() and SessionWidget
 * forwards to SerialConnection::write(). progress() is emitted after every chunk and
 * finished() exactly once per start() (also on cancel()).
 */
class FileSender : public QObject
{
    Q_OBJECT
public:
    enum class Mode { TextLines, Binary };
    Q_ENUM(Mode)

    struct Options
    {
        QString filePath;
        Mode mode = Mode::TextLines;
        int lineDelayMs = 50;                                ///< text mode, 0..60000
        LineEnding::Mode lineEnding = LineEnding::Mode::CR;  ///< text mode
        bool stripLineEndings = true;                        ///< text mode
        bool skipEmptyLines = false;                         ///< text mode
        int chunkSize = 256;                                 ///< binary mode, 1..65536
        int chunkDelayMs = 10;                               ///< binary mode
    };

    explicit FileSender(QObject* parent = nullptr);
    ~FileSender() override;

    /// Opens the file and starts sending. Returns false (with *error) if the file cannot be
    /// read or a send is already running.
    bool start(const Options& options);
    QString lastError() const;

    void pause();
    void resume();
    void cancel();                       ///< emits finished(false, "Cancelled")

    bool isRunning() const;              ///< started and not yet finished (paused counts as running)
    bool isPaused() const;
    Options options() const;

    qint64 totalBytes() const;
    qint64 sentBytes() const;
    int totalLines() const;              ///< text mode only (counted at start), else 0
    int sentLines() const;

signals:
    void chunkReady(const QByteArray& data);
    void progress(qint64 sentBytes, qint64 totalBytes, int sentLines, int totalLines);
    void finished(bool completed, const QString& message);

private slots:
    void sendNext();

private:
    void finish(bool completed, const QString& message);

    Options m_options;
    std::unique_ptr<QFile> m_file;
    QList<QByteArray> m_lines;           ///< text mode: pre-split lines (files are small)
    int m_lineIndex = 0;
    qint64 m_total = 0;
    qint64 m_sent = 0;
    int m_sentLines = 0;
    bool m_running = false;
    bool m_paused = false;
    QString m_error;
    QTimer m_timer;
};
