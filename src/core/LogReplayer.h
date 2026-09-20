#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>
#include <QList>

/**
 * Replays a captured log file through a session as if a device were sending it
 * (File > Replay Log File..., or `--replay <file> [--speed <baud>]`).
 *
 * Use cases: inspect a boot log captured in the field with full colour/cursor rendering,
 * demo the tool, reproduce a rendering bug from a customer capture, and test the whole
 * RX pipeline without hardware.
 *
 * Input formats
 *  - Raw: the file is streamed byte for byte (a SessionLogger "raw" capture, or any text/
 *    ANSI file).
 *  - TimestampedText: a SessionLogger "text" capture. Each line starts with
 *    "[yyyy-MM-dd HH:mm:ss.zzz] "; the prefix is removed, lines that continue with "TX> "
 *    are dropped (they are the host's own input), header lines starting with "# " are
 *    dropped, and the original line break is re-emitted as "\r\n" so the terminal renders
 *    it like the live session did. detectFormat() recognises this format from the first
 *    line.
 *
 * Pacing: bytesPerSecond > 0 streams in chunks every ~20 ms (baud / 10 bytes per second,
 * so "--speed 115200" replays at the real line rate); 0 = as fast as the event loop allows
 * (chunks of 4 KB per timer tick, still asynchronous so the UI stays responsive).
 *
 * Emits chunkReady() for every chunk (SessionWidget feeds it to the terminal, hex view and
 * logger exactly like received data), progress() after every chunk and finished() exactly
 * once per start() (also after stop()).
 */
class LogReplayer : public QObject
{
    Q_OBJECT
public:
    enum class Format { Raw, TimestampedText };
    Q_ENUM(Format)

    struct Options
    {
        QString filePath;
        qint64 bytesPerSecond = 11520;   ///< 115200 baud; 0 = unlimited
        Format format = Format::Raw;
        bool autoDetectFormat = true;    ///< override `format` from the file's first line
        bool loop = false;               ///< restart from the beginning when finished
    };

    explicit LogReplayer(QObject* parent = nullptr);
    ~LogReplayer() override;

    /// Loads the whole file (logs are small; > 64 MiB is refused with an error) and starts
    /// streaming. Returns false and sets lastError() when the file cannot be read or a replay
    /// is already running.
    bool start(const Options& options);
    QString lastError() const;

    void pause();
    void resume();
    void stop();                        ///< emits finished(false)

    bool isRunning() const;             ///< started and not yet finished (paused counts as running)
    bool isPaused() const;
    Options options() const;
    qint64 totalBytes() const;          ///< bytes after format conversion
    qint64 sentBytes() const;
    void setBytesPerSecond(qint64 bytesPerSecond);   ///< live speed change

    /// Recognise a SessionLogger text capture from the first bytes of a file.
    static Format detectFormat(const QByteArray& head);
    /// Convert a TimestampedText capture to the raw byte stream described above.
    static QByteArray stripTimestamps(const QByteArray& text);
    /// Speeds offered in the UI as (label, bytesPerSecond): "9600 baud" .. "1500000 baud", "Unlimited" (0).
    static QList<QPair<QString, qint64>> standardSpeeds();

signals:
    void chunkReady(const QByteArray& data);
    void progress(qint64 sentBytes, qint64 totalBytes);
    void finished(bool completed);

private slots:
    void onTimer();

private:
    void finish(bool completed);

    Options m_options;
    QByteArray m_data;
    qint64 m_pos = 0;
    bool m_running = false;
    bool m_paused = false;
    QString m_error;
    QTimer m_timer;
};
