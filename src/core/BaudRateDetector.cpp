#include "core/BaudRateDetector.h"

#include <QElapsedTimer>
#include <QPointer>
#include <algorithm>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/SerialConnection.h"

namespace {

constexpr int kMinSampleMs = 200;       ///< a candidate is never judged on less than this
constexpr int kMaxSampleMs = 10000;
constexpr int kMinWindowBytes = 64;
constexpr int kMaxWindowBytes = 65536;
constexpr double kGoodScore = 0.90;
constexpr double kBadScore = 0.50;
/// Above this share of 0x00 / 0xFF bytes the score is scaled down: a UART at the wrong rate
/// produces mostly framing-error zeros and idle-line 0xFFs, a text stream almost none.
constexpr double kNullFfTolerance = 0.05;
constexpr double kNullFfPenaltySlope = 2.0;   ///< 30 % null/FF bytes halve the score, 55 % zero it

bool isTextControl(unsigned char b)
{
    return b == '\r' || b == '\n' || b == '\t' || b == 0x1B || b == 0x08 || b == 0x07 || b == 0x0C;
}

/// Length (2..4) of the well-formed UTF-8 sequence starting at p[0]; 0 when p[0] is not a valid
/// lead byte or a continuation byte is out of range (overlongs, surrogates and > U+10FFFF are
/// rejected like a strict decoder does); -1 when the sequence is valid so far but the data ends
/// before it is complete.
int utf8SequenceLength(const unsigned char* p, qsizetype remaining)
{
    const unsigned char lead = p[0];
    int length = 0;
    unsigned char secondMin = 0x80;
    unsigned char secondMax = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) {
            secondMin = 0xA0;
        } else if (lead == 0xED) {
            secondMax = 0x9F;
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) {
            secondMin = 0x90;
        } else if (lead == 0xF4) {
            secondMax = 0x8F;
        }
    } else {
        return 0;
    }
    for (int k = 1; k < length; ++k) {
        if (k >= remaining) {
            return -1;
        }
        const unsigned char c = p[k];
        const unsigned char lo = (k == 1) ? secondMin : 0x80;
        const unsigned char hi = (k == 1) ? secondMax : 0xBF;
        if (c < lo || c > hi) {
            return 0;
        }
    }
    return length;
}

} // namespace

struct BaudRateDetector::Private
{
    QPointer<SerialConnection> connection;
    QList<qint32> candidates;
    int sampleMs = 1500;
    int minBytes = 64;
    int windowBytes = 512;
    int quietMs = 5000;

    // ---- search --------------------------------------------------------------------
    bool running = false;
    bool skipping = false;      ///< the current candidate was rejected by the driver: bytes are ignored
    qint32 originalBaud = 0;
    qint32 current = 0;
    QList<qint32> order;        ///< the current rate first, then the candidates (deduplicated)
    int index = -1;
    QByteArray sample;
    QElapsedTimer sampleClock;
    QTimer sampleTimer;
    bool anyEnough = false;     ///< some candidate produced minBytes bytes (the device is not silent)
    qint32 bestBaud = 0;
    double bestScore = -1.0;
    QMetaObject::Connection dataConnection;
    QMetaObject::Connection stateConnection;

    // ---- watchdog ------------------------------------------------------------------
    QByteArray window;
    QElapsedTimer clock;
    qint64 lastGarbageMs = -1;  ///< when garbageDetected() was last emitted; -1 = no quiet period running
    bool watchdogHeld = false;  ///< a search found the stream unreadable at every rate: silent until it reads well again
};

BaudRateDetector::BaudRateDetector(QObject* parent)
    : QObject(parent)
    , d(new Private)
{
    d->candidates = AppSettings::instance().autoBaudCandidates();
    d->sampleTimer.setSingleShot(true);
    d->sampleTimer.setTimerType(Qt::PreciseTimer);
    connect(&d->sampleTimer, &QTimer::timeout, this, &BaudRateDetector::evaluateCandidate);
    d->clock.start();
}

BaudRateDetector::~BaudRateDetector()
{
    // Not cancel(): the connection may already be gone, and no signal may leave a destructor.
    d->sampleTimer.stop();
    if (d->running) {
        disconnect(d->dataConnection);
        disconnect(d->stateConnection);
    }
    delete d;
}

// ---------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------

double BaudRateDetector::textScore(const QByteArray& data)
{
    if (data.isEmpty()) {
        return 1.0;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData());
    const qsizetype n = data.size();
    double good = 0.0;
    qsizetype nullOrFf = 0;
    qsizetype i = 0;
    while (i < n) {
        const unsigned char b = p[i];
        if (b >= 0x20 && b <= 0x7E) {
            good += 1.0;
            ++i;
            continue;
        }
        if (b < 0x80) {
            // C0 controls and DEL: only the ones a text stream uses are good.
            if (isTextControl(b)) {
                good += 1.0;
            } else if (b == 0x00) {
                ++nullOrFf;
            }
            ++i;
            continue;
        }
        if (b == 0xFF) {
            ++nullOrFf;
            ++i;
            continue;
        }
        const int length = utf8SequenceLength(p + i, n - i);
        if (length > 0) {
            good += length;
            i += length;
            continue;
        }
        if (length < 0) {
            // A multi-byte character cut off by the end of the data (the last bytes of a chunk):
            // the incomplete sequence counts as one bad byte, not one per byte.
            const qsizetype rest = n - i;
            good += static_cast<double>(rest - 1);
            i = n;
            continue;
        }
        ++i;   // stray continuation byte, overlong / invalid lead: bad
    }
    double score = good / static_cast<double>(n);
    const double nullShare = static_cast<double>(nullOrFf) / static_cast<double>(n);
    if (nullShare > kNullFfTolerance) {
        score *= std::max(0.0, 1.0 - (nullShare - kNullFfTolerance) * kNullFfPenaltySlope);
    }
    return std::clamp(score, 0.0, 1.0);
}

double BaudRateDetector::goodScore()
{
    return kGoodScore;
}

double BaudRateDetector::badScore()
{
    return kBadScore;
}

// ---------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------

void BaudRateDetector::setConnection(SerialConnection* connection)
{
    if (d->running) {
        qCWarning(lcSerial) << "setConnection() while a baud rate detection runs: cancelling it";
        cancel();
    }
    d->connection = connection;
}

SerialConnection* BaudRateDetector::connection() const
{
    return d->connection;
}

void BaudRateDetector::setCandidates(const QList<qint32>& candidates)
{
    d->candidates = candidates;
}

QList<qint32> BaudRateDetector::candidates() const
{
    return d->candidates;
}

void BaudRateDetector::setSampleMs(int ms)
{
    d->sampleMs = std::clamp(ms, kMinSampleMs, kMaxSampleMs);
}

int BaudRateDetector::sampleMs() const
{
    return d->sampleMs;
}

void BaudRateDetector::setMinBytes(int bytes)
{
    d->minBytes = std::max(1, bytes);
}

int BaudRateDetector::minBytes() const
{
    return d->minBytes;
}

bool BaudRateDetector::isRunning() const
{
    return d->running;
}

qint32 BaudRateDetector::currentCandidate() const
{
    return d->running ? d->current : 0;
}

void BaudRateDetector::setWindowBytes(int bytes)
{
    d->windowBytes = std::clamp(bytes, kMinWindowBytes, kMaxWindowBytes);
    if (d->window.size() > d->windowBytes) {
        d->window.remove(0, d->window.size() - d->windowBytes);
    }
}

void BaudRateDetector::setQuietMs(int ms)
{
    d->quietMs = std::max(0, ms);
}

void BaudRateDetector::resetWatchdog()
{
    d->window.clear();
    d->lastGarbageMs = -1;
}

bool BaudRateDetector::isWatchdogHeld() const
{
    return d->watchdogHeld;
}

// ---------------------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------------------

bool BaudRateDetector::start()
{
    if (d->running) {
        qCDebug(lcSerial) << "baud rate detection already running";
        return false;
    }
    if (!d->connection || !d->connection->isOpen()) {
        qCDebug(lcSerial) << "baud rate detection not started: no open connection";
        return false;
    }
    if (d->candidates.isEmpty()) {
        qCDebug(lcSerial) << "baud rate detection not started: no candidates";
        return false;
    }

    d->running = true;
    d->skipping = false;
    d->originalBaud = d->connection->settings().baudRate;
    d->order.clear();
    d->order.append(d->originalBaud);
    for (const qint32 candidate : d->candidates) {
        if (SerialSettings::isValidBaudRate(candidate) && !d->order.contains(candidate)) {
            d->order.append(candidate);
        }
    }
    d->index = -1;
    d->current = 0;
    d->anyEnough = false;
    d->bestBaud = d->originalBaud;
    d->bestScore = -1.0;
    d->sample.clear();
    d->watchdogHeld = false;   // a new search judges the stream afresh; its outcome decides again

    d->dataConnection = connect(d->connection, &Transport::dataReceived, this, &BaudRateDetector::onSampleData);
    d->stateConnection = connect(d->connection, &Transport::stateChanged, this, [this](Transport::State state) {
        if (d->running && state != Transport::State::Connected) {
            qCInfo(lcSerial) << "baud rate detection cancelled: connection" << Transport::stateText(state);
            cancel();
        }
    });

    qCInfo(lcSerial) << "baud rate detection started on" << d->connection->portName() << "- current" << d->originalBaud
                     << "candidates" << d->order << "sample" << d->sampleMs << "ms, min" << d->minBytes << "bytes";
    emit started(d->originalBaud);
    if (d->running) {   // a slot of started() may have cancelled
        beginCandidate(0);
    }
    return true;
}

void BaudRateDetector::beginCandidate(int index)
{
    d->index = index;
    d->current = d->order.at(index);
    d->skipping = false;
    d->sample.clear();
    SerialConnection* connection = d->connection;
    if (connection->settings().baudRate != d->current) {
        SerialSettings settings = connection->settings();
        settings.baudRate = d->current;
        connection->setSettings(settings);
        if (connection->settings().baudRate != d->current) {
            // The driver rejected the rate (reported through errorOccurred() by the connection):
            // judged as "nothing received" on the next event-loop turn so the candidateTried()
            // order stays the order of the list.
            qCWarning(lcSerial) << "baud rate candidate" << d->current << "rejected by the driver, skipped";
            d->skipping = true;
            d->sampleClock.start();
            d->sampleTimer.start(0);
            return;
        }
    }
    // Only the RX side: what the driver still holds was received at the previous rate and must
    // not be judged against this one. The TX side is left alone - purging it would drop the
    // bytes the session queued (a running file send) and, on Windows, abort a write in flight
    // whose late error looks like a vanished device (SerialConnection::clearBuffers()).
    connection->clearBuffers(QSerialPort::Input);
    d->sampleClock.start();
    d->sampleTimer.start(d->sampleMs);
    qCDebug(lcSerial) << "trying" << d->current << "baud";
}

void BaudRateDetector::onSampleData(const QByteArray& data)
{
    if (!d->running || d->skipping || data.isEmpty()) {
        return;
    }
    d->sample += data;
    if (d->sample.size() < d->minBytes) {
        return;
    }
    // Enough bytes to judge: do it now, or as soon as the minimum listening time is over.
    const qint64 elapsed = d->sampleClock.elapsed();
    if (elapsed >= kMinSampleMs) {
        evaluateCandidate();
    } else {
        const int remaining = static_cast<int>(kMinSampleMs - elapsed);
        if (d->sampleTimer.remainingTime() > remaining) {
            d->sampleTimer.start(remaining);
        }
    }
}

void BaudRateDetector::evaluateCandidate()
{
    if (!d->running) {
        return;
    }
    d->sampleTimer.stop();
    const qint32 tried = d->current;
    const int bytes = static_cast<int>(d->sample.size());
    const double score = bytes > 0 ? textScore(d->sample) : 0.0;
    const bool enough = bytes >= d->minBytes;
    d->sample.clear();
    qCInfo(lcSerial) << "baud" << tried << ":" << bytes << "bytes, text score" << score
                     << (enough ? "" : "(not enough bytes)");

    if (enough) {
        d->anyEnough = true;
        if (score > d->bestScore) {
            d->bestScore = score;
            d->bestBaud = tried;
        }
        if (score >= kGoodScore) {
            // Found: the rate stays applied. currentCandidate() is 0 from here on.
            d->current = 0;
            emit candidateTried(tried, score, bytes);
            finishSearch(true, tried, score);
            return;
        }
    }

    const int next = d->index + 1;
    if (next < d->order.size()) {
        // Move on first so that currentCandidate() already names the next candidate when the
        // listeners of candidateTried() run (SessionWidget shows "Trying <next>...").
        beginCandidate(next);
        emit candidateTried(tried, score, bytes);
        return;
    }

    d->current = 0;
    emit candidateTried(tried, score, bytes);
    if (!d->running) {
        return;   // a listener cancelled
    }
    if (!d->anyEnough) {
        finishSearch(false, d->originalBaud, 0.0);
    } else if (d->bestScore >= kGoodScore) {
        finishSearch(true, d->bestBaud, d->bestScore);
    } else {
        // Bytes arrived but nothing readable at any candidate (a rate outside the list, a
        // binary protocol, a floating line): another watchdog report would only start the
        // same search again, muting the session for its whole duration, so the watchdog stays
        // quiet until the stream reads well again or a search is started by other means.
        d->watchdogHeld = true;
        finishSearch(false, d->bestBaud, d->bestScore);
    }
}

void BaudRateDetector::finishSearch(bool found, qint32 baud, double score)
{
    d->running = false;
    d->skipping = false;
    d->current = 0;
    d->sampleTimer.stop();
    d->sample.clear();
    disconnect(d->dataConnection);
    disconnect(d->stateConnection);

    const qint32 target = found ? baud : d->originalBaud;
    if (d->connection && d->connection->settings().baudRate != target) {
        // The best candidate is applied (or the original rate restored). A closed connection
        // simply stores the value for its next open().
        SerialSettings settings = d->connection->settings();
        settings.baudRate = target;
        d->connection->setSettings(settings);
    }
    resetWatchdog();   // whatever was scored before the rate changed is stale

    if (found) {
        qCInfo(lcSerial) << "baud rate detected:" << baud << "(score" << score << ")";
    } else if (d->watchdogHeld) {
        // Set by evaluateCandidate() for the "bytes but nothing readable" outcome.
        qCInfo(lcSerial) << "baud rate detection: nothing readable (best" << baud << "score" << score << "), keeping"
                         << target << "- watchdog held until readable output arrives";
    } else {
        qCInfo(lcSerial) << "baud rate detection: no output at any rate (or cancelled), keeping" << target;
    }
    emit finished(found, baud, score);
}

void BaudRateDetector::cancel()
{
    if (!d->running) {
        return;
    }
    qCInfo(lcSerial) << "baud rate detection cancelled, restoring" << d->originalBaud;
    finishSearch(false, d->originalBaud, 0.0);
}

// ---------------------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------------------

void BaudRateDetector::feed(const QByteArray& data)
{
    if (d->running || data.isEmpty()) {
        return;
    }
    d->window += data;
    if (d->window.size() > d->windowBytes) {
        d->window.remove(0, d->window.size() - d->windowBytes);
    }
    if (d->window.size() < d->windowBytes) {
        return;
    }
    const double score = textScore(d->window);
    if (score >= kBadScore) {
        d->lastGarbageMs = -1;   // readable again: the next garbage window may report at once
        if (d->watchdogHeld) {
            d->watchdogHeld = false;
            qCInfo(lcSerial) << "baud rate watchdog: readable output again, watchdog released";
        }
        return;
    }
    if (d->watchdogHeld) {
        return;   // the last search already found every candidate unreadable: no storm
    }
    const qint64 now = d->clock.elapsed();
    if (d->lastGarbageMs >= 0 && now - d->lastGarbageMs < d->quietMs) {
        return;   // still within the quiet period of the last report
    }
    d->lastGarbageMs = now;
    qCInfo(lcSerial) << "baud rate watchdog: incoming bytes are unreadable (score" << score << ")";
    emit garbageDetected(score);
}
