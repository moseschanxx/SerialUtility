#include "core/LogReplayer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include "app/Logging.h"

namespace {

constexpr int kTickMs = 20;
constexpr int kTicksPerSecond = 1000 / kTickMs;
constexpr qint64 kUnlimitedChunkBytes = 4096;
constexpr qint64 kUnlimitedSliceMs = 8;   ///< unlimited speed: feed chunks for this long per event-loop iteration
constexpr qint64 kMaxFileBytes = 64LL * 1024 * 1024;
constexpr qint64 kDetectHeadBytes = 4096;
constexpr int kTimestampPrefixLength = 26;   ///< "[yyyy-MM-dd HH:mm:ss.zzz] "
const char kHeaderPrefix[] = "# BuildAI Serial Utility log";

/// True when `line` starts with a SessionLogger text-format timestamp prefix.
bool hasTimestampPrefix(const QByteArray& line)
{
    static const char pattern[] = "[dddd-dd-dd dd:dd:dd.ddd] ";
    if (line.size() < kTimestampPrefixLength) {
        return false;
    }
    for (int i = 0; i < kTimestampPrefixLength; ++i) {
        const char p = pattern[i];
        const char c = line.at(i);
        if (p == 'd') {
            if (c < '0' || c > '9') {
                return false;
            }
        } else if (c != p) {
            return false;
        }
    }
    return true;
}

} // namespace

LogReplayer::LogReplayer(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(kTickMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &LogReplayer::onTimer);
}

LogReplayer::~LogReplayer() = default;

bool LogReplayer::start(const Options& options)
{
    if (m_running) {
        m_error = tr("A replay is already running");
        return false;
    }
    const QString path = options.filePath;
    if (path.trimmed().isEmpty()) {
        m_error = tr("No file selected");
        return false;
    }
    QFile file(path);
    if (!file.exists()) {
        m_error = tr("File not found: %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (file.size() > kMaxFileBytes) {
        m_error = tr("%1 is too large to replay (%2 MiB, limit %3 MiB)")
                      .arg(QDir::toNativeSeparators(path))
                      .arg(static_cast<double>(file.size()) / (1024.0 * 1024.0), 0, 'f', 1)
                      .arg(kMaxFileBytes / (1024 * 1024));
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = tr("Cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    QByteArray data = file.readAll();
    file.close();

    m_options = options;
    m_options.bytesPerSecond = qMax<qint64>(0, m_options.bytesPerSecond);
    applyInterval();
    if (m_options.autoDetectFormat) {
        m_options.format = detectFormat(data.left(kDetectHeadBytes));
    }
    if (m_options.format == Format::TimestampedText) {
        data = stripTimestamps(data);
    }

    m_data = data;
    m_pos = 0;
    m_error.clear();
    m_running = true;
    m_paused = false;
    m_timer.start();
    qCInfo(lcApp) << "replaying" << QDir::toNativeSeparators(path) << m_data.size() << "bytes at"
                  << (m_options.bytesPerSecond > 0 ? QString::number(m_options.bytesPerSecond) + QStringLiteral(" B/s")
                                                   : QStringLiteral("unlimited speed"))
                  << (m_options.format == Format::TimestampedText ? "(timestamped text capture)" : "(raw)")
                  << (m_options.loop ? "looping" : "");
    return true;
}

QString LogReplayer::lastError() const
{
    return m_error;
}

void LogReplayer::pause()
{
    if (!m_running || m_paused) {
        return;
    }
    m_paused = true;
    m_timer.stop();
    qCDebug(lcApp) << "replay paused at" << m_pos << "/" << m_data.size();
}

void LogReplayer::resume()
{
    if (!m_running || !m_paused) {
        return;
    }
    m_paused = false;
    m_timer.start();
    qCDebug(lcApp) << "replay resumed";
}

void LogReplayer::stop()
{
    if (!m_running) {
        return;
    }
    qCInfo(lcApp) << "replay stopped at" << m_pos << "/" << m_data.size();
    finish(false);
}

bool LogReplayer::isRunning() const
{
    return m_running;
}

bool LogReplayer::isPaused() const
{
    return m_running && m_paused;
}

LogReplayer::Options LogReplayer::options() const
{
    return m_options;
}

qint64 LogReplayer::totalBytes() const
{
    return m_data.size();
}

qint64 LogReplayer::sentBytes() const
{
    return m_pos;
}

void LogReplayer::setBytesPerSecond(qint64 bytesPerSecond)
{
    m_options.bytesPerSecond = qMax<qint64>(0, bytesPerSecond);
    applyInterval();
}

void LogReplayer::applyInterval()
{
    // Paced: one chunk every 20 ms. Unlimited (0): a zero-interval timer fires once per
    // event-loop iteration after pending window-system events, so the UI stays responsive.
    // QTimer::setInterval() restarts an active timer, so a live change applies immediately;
    // while paused the timer is stopped and resume() picks up the stored interval.
    const int interval = m_options.bytesPerSecond > 0 ? kTickMs : 0;
    if (m_timer.interval() != interval) {
        m_timer.setInterval(interval);
    }
}

LogReplayer::Format LogReplayer::detectFormat(const QByteArray& head)
{
    qsizetype pos = 0;
    while (pos < head.size()) {
        const qsizetype end = head.indexOf('\n', pos);
        QByteArray line = end < 0 ? head.mid(pos) : head.mid(pos, end - pos);
        pos = end < 0 ? head.size() : end + 1;
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.trimmed().isEmpty()) {
            continue;   // skip leading blank lines
        }
        if (hasTimestampPrefix(line) || line.startsWith(kHeaderPrefix)) {
            return Format::TimestampedText;
        }
        return Format::Raw;
    }
    return Format::Raw;
}

QByteArray LogReplayer::stripTimestamps(const QByteArray& text)
{
    QByteArray out;
    out.reserve(text.size());
    bool pendingLf = false;   // previous kept line ended in a bare '\n'; break not yet emitted
    qsizetype pos = 0;
    while (pos < text.size()) {
        const qsizetype newline = text.indexOf('\n', pos);
        const bool hasBreak = newline >= 0;
        QByteArray line = hasBreak ? text.mid(pos, newline - pos) : text.mid(pos);
        pos = hasBreak ? newline + 1 : text.size();
        const bool hadCr = line.endsWith('\r');
        if (hadCr) {
            line.chop(1);
        }
        const bool hadPrefix = hasTimestampPrefix(line);
        bool isTx = false;
        if (hadPrefix) {
            line.remove(0, kTimestampPrefixLength);
            isTx = line.startsWith("TX> ");
        }
        if (isTx) {
            // The host's own input. SessionLogger::logSent() inserts a bare '\n' before a TX
            // line when RX left the cursor mid-line (a prompt); the device never sent that
            // break, so swallow it. A device CRLF carries the '\r' and was emitted right away.
            pendingLf = false;
            continue;
        }
        if (pendingLf) {
            out += "\r\n";
            pendingLf = false;
        }
        if (!hadPrefix && line.startsWith("# ")) {
            continue;   // header line written by SessionLogger (a timestamped "# " is a root prompt)
        }
        out += line;
        if (hasBreak) {
            if (hadCr) {
                out += "\r\n";
            } else {
                pendingLf = true;
            }
        }
    }
    if (pendingLf) {
        out += "\r\n";
    }
    return out;
}

QList<QPair<QString, qint64>> LogReplayer::standardSpeeds()
{
    QList<QPair<QString, qint64>> speeds;
    for (qint64 baud : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000}) {
        speeds.append(qMakePair(tr("%1 baud").arg(baud), baud / 10));
    }
    speeds.append(qMakePair(tr("Unlimited"), qint64(0)));
    return speeds;
}

void LogReplayer::onTimer()
{
    // Paced: one chunk per tick. Unlimited: 4 KB chunks until the slice is used up (or a
    // receiver stopped/paused us or switched to a paced speed), then yield to the event loop;
    // the zero-interval timer fires again on the next iteration.
    QElapsedTimer slice;
    slice.start();
    do {
        if (!m_running || m_paused) {
            return;
        }
        if (m_pos >= m_data.size()) {
            if (m_options.loop && !m_data.isEmpty()) {
                m_pos = 0;
            } else {
                finish(true);
                return;
            }
        }
        const qint64 bytesPerSecond = m_options.bytesPerSecond;
        const qint64 chunkSize =
            bytesPerSecond <= 0 ? kUnlimitedChunkBytes : qMax<qint64>(1, bytesPerSecond / kTicksPerSecond);
        const qint64 count = qMin<qint64>(chunkSize, m_data.size() - m_pos);
        const QByteArray chunk = m_data.mid(m_pos, count);
        m_pos += count;
        emit chunkReady(chunk);
        emit progress(m_pos, m_data.size());
        if (m_pos >= m_data.size() && !m_options.loop) {
            finish(true);
            return;
        }
    } while (m_options.bytesPerSecond <= 0 && slice.elapsed() < kUnlimitedSliceMs);
}

void LogReplayer::finish(bool completed)
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_paused = false;
    m_timer.stop();
    if (completed) {
        qCInfo(lcApp) << "replay finished:" << m_data.size() << "bytes";
    }
    emit finished(completed);
}
