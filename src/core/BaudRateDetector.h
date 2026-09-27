#pragma once

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QTimer>

class SerialConnection;

/**
 * Finds the baud rate a serial device is talking at by listening to what it sends.
 *
 * A wrong baud rate turns a boot log into a stream of 0xFF/0x00 bytes and high-bit garbage; a
 * right one yields text. textScore() measures that: the fraction of bytes that are printable
 * ASCII, common control characters (CR, LF, TAB, ESC, BS, BEL, FF) or part of a well-formed
 * UTF-8 multi-byte sequence, minus a penalty for 0x00 / 0xFF runs and invalid UTF-8. A
 * Rockchip boot log at the right rate scores > 0.97; the same log at the wrong rate scores < 0.4.
 *
 * Detection (start()) runs on an OPEN SerialConnection and drives it through the candidate
 * list: for each candidate it applies the rate live (SerialConnection::setSettings() keeps the
 * port open), clears the RX buffer only (SerialConnection::clearBuffers(QSerialPort::Input):
 * bytes the session queued for transmission keep going out, and no write in flight is
 * aborted), listens for sampleMs (or until minBytes arrived, whichever
 * first, but at least 200 ms), scores what came in and moves on. The current rate of the
 * connection is tried first, so a session that is already right is confirmed without ever
 * changing the rate. A candidate scoring >= goodScore() with >= minBytes bytes ends the search
 * immediately (finished(true, baud, score)); otherwise the best-scoring candidate wins when it
 * scored >= goodScore(); when no candidate produced minBytes bytes the device was silent
 * (finished(false, originalBaud, 0) and the original rate is restored); when bytes arrived but
 * nothing scored well enough, finished(false, bestBaud, bestScore) and the original rate is
 * restored as well. The bytes read during the search are NOT forwarded through
 * dataReceived() by this class - the connection emits them as usual; the session decides what
 * to do with them (SessionWidget mutes the terminal while detecting and shows a status line).
 *
 * cancel() restores the original rate and emits finished(false, originalBaud, 0). The
 * connection closing during the search cancels it. Detection never writes to the device.
 * A candidate the driver rejects (SerialConnection::setSettings() kept the previous rate) is
 * reported as candidateTried(baud, 0, 0) and skipped. The search moves on BEFORE it reports:
 * when candidateTried() is emitted, currentCandidate() already names the candidate now being
 * listened to (SessionWidget shows "Trying <next>..."), and 0 when the search is about to end
 * - with the reported candidate as the winner, or with finished(false, ...).
 *
 * Watchdog (SessionWidget uses it while the "Auto" rate is selected): feed() accumulates the
 * live RX stream in a sliding window of windowBytes (default 512); once the window is full and
 * scores below badScore() (default 0.5) garbageDetected() is emitted, at most once per
 * quietMs (default 5000 ms) so a flapping line does not trigger a detection storm; a window
 * that scores well again resets the count. Hold: a search that ends with bytes received but
 * nothing readable at any candidate (some candidate produced minBytes bytes, none reached
 * goodScore(): finished(false, bestBaud, bestScore) - a rate outside the list, a binary
 * protocol, a floating line) puts the watchdog on hold (isWatchdogHeld()):
 * garbage windows are ignored, so the same failing search is not run again every quiet
 * period with the session muted for its whole duration. The hold ends when a full window
 * scores >= badScore() again (the stream is readable: a wrong rate later is reported as
 * usual) or when start() runs a search by other means (a manual Detect Baud Rate, a switch to
 * "Auto"). A silent device (score 0) and cancel() never hold the watchdog.
 */
class BaudRateDetector : public QObject
{
    Q_OBJECT
public:
    explicit BaudRateDetector(QObject* parent = nullptr);
    ~BaudRateDetector() override;

    /// 0.0 (pure garbage) .. 1.0 (clean text); 1.0 for an empty array. UTF-8 aware.
    static double textScore(const QByteArray& data);
    static double goodScore();     ///< 0.90
    static double badScore();      ///< 0.50

    void setConnection(SerialConnection* connection);   ///< not owned; must be open when start() is called
    SerialConnection* connection() const;

    void setCandidates(const QList<qint32>& candidates);   ///< AppSettings::autoBaudCandidates() by default
    QList<qint32> candidates() const;
    void setSampleMs(int ms);                             ///< default 1500, clamp 200..10000
    int sampleMs() const;
    void setMinBytes(int bytes);                          ///< default 64
    int minBytes() const;

    bool isRunning() const;
    qint32 currentCandidate() const;                      ///< 0 when idle (and while candidateTried() reports the last one)

    // ---- Watchdog ---------------------------------------------------------------------
    void setWindowBytes(int bytes);                       ///< default 512, clamp 64..65536
    void setQuietMs(int ms);                              ///< default 5000
    void resetWatchdog();                                 ///< forget the window (after a detection); a hold stays
    bool isWatchdogHeld() const;                          ///< see "Hold" in the class comment

public slots:
    /// Start the search; returns false (and emits nothing) when no connection is set, it is not
    /// open, a search is already running, or the candidate list is empty.
    bool start();
    void cancel();
    /// Live RX bytes for the watchdog (see class comment). Ignored while a search runs.
    void feed(const QByteArray& data);

signals:
    void started(qint32 originalBaud);
    void candidateTried(qint32 baud, double score, int bytes);
    void finished(bool found, qint32 baud, double score);
    void garbageDetected(double score);

private:
    struct Private;
    Private* d;

    void beginCandidate(int index);         ///< apply order[index] live (skipped when the driver rejects it) and sample it
    void evaluateCandidate();               ///< score the sample: early exit, next candidate, or the end of the search
    void finishSearch(bool found, qint32 baud, double score);   ///< leave/restore the rate, emit finished()
    void onSampleData(const QByteArray& data);
};
