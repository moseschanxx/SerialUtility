#include "core/FileSender.h"

#include <QFileInfo>
#include <QDir>

#include "app/Logging.h"

namespace {

constexpr qint64 kLargeTextFileBytes = 16LL * 1024 * 1024;   // warn above this in text mode
constexpr int kMaxLineDelayMs = 60000;
constexpr int kMinChunkSize = 1;
constexpr int kMaxChunkSize = 65536;

/// Split `content` into lines on \r\n, \n or \r. Each element is {content, ending}.
struct RawLine
{
    QByteArray text;
    QByteArray ending;
};

QList<RawLine> splitLines(const QByteArray& content)
{
    QList<RawLine> lines;
    const qsizetype n = content.size();
    qsizetype start = 0;
    qsizetype i = 0;
    while (i < n) {
        const char c = content.at(i);
        if (c == '\n') {
            lines.append({content.mid(start, i - start), QByteArrayLiteral("\n")});
            ++i;
            start = i;
        } else if (c == '\r') {
            if (i + 1 < n && content.at(i + 1) == '\n') {
                lines.append({content.mid(start, i - start), QByteArrayLiteral("\r\n")});
                i += 2;
            } else {
                lines.append({content.mid(start, i - start), QByteArrayLiteral("\r")});
                ++i;
            }
            start = i;
        } else {
            ++i;
        }
    }
    if (start < n) {
        lines.append({content.mid(start), QByteArray()});   // last line without a line break
    }
    return lines;
}

} // namespace

FileSender::FileSender(QObject* parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &FileSender::sendNext);
}

FileSender::~FileSender()
{
    m_timer.stop();
    m_running = false;
    m_paused = false;
}

bool FileSender::start(const Options& options)
{
    m_error.clear();
    if (m_running) {
        m_error = tr("A file transfer is already running");
        qCWarning(lcSerial) << m_error;
        return false;
    }

    Options opts = options;
    opts.lineDelayMs = qBound(0, opts.lineDelayMs, kMaxLineDelayMs);
    opts.chunkSize = qBound(kMinChunkSize, opts.chunkSize, kMaxChunkSize);
    opts.chunkDelayMs = qMax(0, opts.chunkDelayMs);

    auto file = std::make_unique<QFile>(opts.filePath);
    if (opts.filePath.isEmpty() || !file->open(QIODevice::ReadOnly)) {
        m_error = tr("Cannot read %1: %2")
                      .arg(QDir::toNativeSeparators(opts.filePath),
                           opts.filePath.isEmpty() ? tr("no file selected") : file->errorString());
        qCWarning(lcSerial) << m_error;
        return false;
    }

    m_options = opts;
    m_lines.clear();
    m_lineIndex = 0;
    m_sent = 0;
    m_sentLines = 0;
    m_total = 0;

    if (opts.mode == Mode::TextLines) {
        if (file->size() > kLargeTextFileBytes) {
            qCWarning(lcSerial) << "text file" << QDir::toNativeSeparators(opts.filePath) << "is"
                                << file->size() << "bytes; loading it entirely into memory";
        }
        const QByteArray content = file->readAll();
        file->close();
        m_file.reset();

        const QByteArray ending = LineEnding::bytes(opts.lineEnding);
        const QList<RawLine> raw = splitLines(content);
        m_lines.reserve(raw.size());
        for (const RawLine& line : raw) {
            if (opts.skipEmptyLines && line.text.trimmed().isEmpty()) {
                continue;
            }
            QByteArray payload = line.text;
            if (!opts.stripLineEndings) {
                payload.append(line.ending);
            }
            payload.append(ending);
            m_total += payload.size();
            m_lines.append(payload);
        }
        qCInfo(lcSerial) << "sending" << QDir::toNativeSeparators(opts.filePath) << "as" << m_lines.size()
                         << "lines (" << m_total << "bytes), delay" << opts.lineDelayMs << "ms";
    } else {
        m_total = file->size();
        m_file = std::move(file);
        qCInfo(lcSerial) << "sending" << QDir::toNativeSeparators(opts.filePath) << "as binary," << m_total
                         << "bytes in chunks of" << opts.chunkSize << "delay" << opts.chunkDelayMs << "ms";
    }

    m_running = true;
    m_paused = false;
    // Always yield through the event loop so finished()/progress() never fire from inside start().
    m_timer.start(0);
    return true;
}

QString FileSender::lastError() const
{
    return m_error;
}

void FileSender::pause()
{
    if (!m_running || m_paused) {
        return;
    }
    m_paused = true;
    m_timer.stop();
    qCDebug(lcSerial) << "file send paused at" << m_sent << "/" << m_total;
}

void FileSender::resume()
{
    if (!m_running || !m_paused) {
        return;
    }
    m_paused = false;
    qCDebug(lcSerial) << "file send resumed";
    m_timer.start(0);
}

void FileSender::cancel()
{
    if (!m_running) {
        return;
    }
    qCInfo(lcSerial) << "file send cancelled at" << m_sent << "/" << m_total;
    finish(false, tr("Cancelled"));
}

bool FileSender::isRunning() const
{
    return m_running;
}

bool FileSender::isPaused() const
{
    return m_running && m_paused;
}

FileSender::Options FileSender::options() const
{
    return m_options;
}

qint64 FileSender::totalBytes() const
{
    return m_total;
}

qint64 FileSender::sentBytes() const
{
    return m_sent;
}

int FileSender::totalLines() const
{
    return m_options.mode == Mode::TextLines ? static_cast<int>(m_lines.size()) : 0;
}

int FileSender::sentLines() const
{
    return m_sentLines;
}

void FileSender::sendNext()
{
    if (!m_running || m_paused) {
        return;
    }

    if (m_options.mode == Mode::TextLines) {
        if (m_lineIndex >= m_lines.size()) {
            finish(true, tr("Sent %n line(s), %1 bytes", nullptr, m_sentLines).arg(m_sent));
            return;
        }
        const QByteArray chunk = m_lines.at(m_lineIndex);
        ++m_lineIndex;
        ++m_sentLines;
        m_sent += chunk.size();
        emit chunkReady(chunk);
        emit progress(m_sent, m_total, m_sentLines, static_cast<int>(m_lines.size()));
        if (!m_running) {   // a slot cancelled us
            return;
        }
        if (m_lineIndex >= m_lines.size()) {
            finish(true, tr("Sent %n line(s), %1 bytes", nullptr, m_sentLines).arg(m_sent));
        } else {
            m_timer.start(m_options.lineDelayMs);
        }
        return;
    }

    // Binary mode
    if (!m_file || !m_file->isOpen()) {
        finish(false, tr("File is no longer readable"));
        return;
    }
    const QByteArray chunk = m_file->read(m_options.chunkSize);
    if (chunk.isEmpty()) {
        if (m_file->error() != QFile::NoError) {
            finish(false, tr("Read error: %1").arg(m_file->errorString()));
        } else {
            finish(true, tr("Sent %1 bytes").arg(m_sent));
        }
        return;
    }
    m_sent += chunk.size();
    emit chunkReady(chunk);
    emit progress(m_sent, m_total, 0, 0);
    if (!m_running) {
        return;
    }
    if (m_file->atEnd() || m_sent >= m_total) {
        finish(true, tr("Sent %1 bytes").arg(m_sent));
    } else {
        m_timer.start(m_options.chunkDelayMs);
    }
}

void FileSender::finish(bool completed, const QString& message)
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_paused = false;
    m_timer.stop();
    if (m_file) {
        m_file->close();
        m_file.reset();
    }
    qCInfo(lcSerial) << "file send finished:" << message << (completed ? "" : "(not completed)");
    emit finished(completed, message);
}
