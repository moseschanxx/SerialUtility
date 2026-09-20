#include "core/SessionLogger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include "app/Logging.h"
#include "core/HexUtils.h"

namespace {

QByteArray stampPrefix(const QString& timestamp)
{
    return QByteArrayLiteral("[") + timestamp.toUtf8() + QByteArrayLiteral("] ");
}

} // namespace

SessionLogger::SessionLogger(QObject* parent)
    : QObject(parent)
{
}

SessionLogger::~SessionLogger()
{
    stop();
}

bool SessionLogger::start(const QString& filePath, Format format, bool includeTx, const QString& header)
{
    if (m_file) {
        stop();
    }
    if (filePath.trimmed().isEmpty()) {
        const QString message = tr("No log file name given");
        qCWarning(lcSerial) << message;
        emit error(message);
        return false;
    }

    const QFileInfo info(filePath);
    const QDir dir = info.dir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        const QString message =
            tr("Cannot create log directory %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
        qCWarning(lcSerial) << message;
        emit error(message);
        return false;
    }

    auto file = std::make_unique<QFile>(filePath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
        const QString message =
            tr("Cannot open log file %1: %2").arg(QDir::toNativeSeparators(filePath), file->errorString());
        qCWarning(lcSerial) << message;
        emit error(message);
        return false;
    }

    m_file = std::move(file);
    m_format = format;
    m_includeTx = includeTx;
    m_atLineStart = true;
    m_bytesWritten = 0;

    if (m_format != Format::Raw) {
        // Appending to an existing capture: the previous log usually ends in an unterminated
        // prompt line, so make sure the header starts on a fresh line (Raw stays verbatim).
        bool needsNewline = false;
        const qint64 existing = m_file->size();
        if (existing > 0) {
            // Separate read-only handle: the Append-mode handle has no useful seek semantics.
            QFile tail(filePath);
            if (tail.open(QIODevice::ReadOnly) && tail.seek(existing - 1)) {
                char last = '\n';
                if (tail.read(&last, 1) == 1) {
                    needsNewline = (last != '\n');
                }
            }
        }

        QString line = QStringLiteral("# BuildAI Serial Utility log");
        if (!header.trimmed().isEmpty()) {
            line += QStringLiteral(" - ") + header.trimmed();
        }
        line += QStringLiteral(" - started %1\n").arg(timestamp());
        QByteArray bytes = line.toUtf8();
        if (needsNewline) {
            bytes.prepend('\n');
        }
        writeRaw(bytes);
    }
    if (!m_file) {
        // writeRaw() failed: it already called stop() and emitted error(); do not report success.
        return false;
    }

    qCInfo(lcSerial) << "logging to" << QDir::toNativeSeparators(filePath) << "format" << formatToString(m_format)
                     << "includeTx" << m_includeTx;
    emit started(filePath);
    return true;
}

void SessionLogger::stop()
{
    if (!m_file) {
        return;
    }
    const QString path = m_file->fileName();
    const qint64 bytes = m_bytesWritten;
    m_file->flush();
    m_file->close();
    m_file.reset();
    qCInfo(lcSerial) << "log closed" << QDir::toNativeSeparators(path) << bytes << "bytes";
    emit stopped(path, bytes);
}

bool SessionLogger::isActive() const
{
    return m_file != nullptr && m_file->isOpen();
}

QString SessionLogger::filePath() const
{
    return m_file ? m_file->fileName() : QString();
}

SessionLogger::Format SessionLogger::format() const
{
    return m_format;
}

qint64 SessionLogger::bytesWritten() const
{
    return m_bytesWritten;
}

QString SessionLogger::suggestFileName(const QString& portName, const QString& directory)
{
    QString port = portName.trimmed();
    port.replace(QLatin1Char(':'), QLatin1Char('_'));
    port.replace(QLatin1Char('/'), QLatin1Char('_'));
    port.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (port.isEmpty()) {
        port = QStringLiteral("serial");
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const QString name = QStringLiteral("%1_%2.log").arg(port, stamp);
    if (directory.isEmpty()) {
        return name;
    }
    return QDir(directory).filePath(name);
}

QString SessionLogger::formatToString(Format format)
{
    switch (format) {
    case Format::Raw:
        return QStringLiteral("raw");
    case Format::HexDump:
        return QStringLiteral("hex");
    case Format::Text:
        break;
    }
    return QStringLiteral("text");
}

SessionLogger::Format SessionLogger::formatFromString(const QString& key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("raw")) {
        return Format::Raw;
    }
    if (k == QLatin1String("hex") || k == QLatin1String("hexdump")) {
        return Format::HexDump;
    }
    return Format::Text;
}

void SessionLogger::logReceived(const QByteArray& data)
{
    if (!isActive() || data.isEmpty()) {
        return;
    }
    switch (m_format) {
    case Format::Raw:
        writeRaw(data);
        break;
    case Format::Text: {
        const QByteArray prefix = stampPrefix(timestamp());
        QByteArray out;
        out.reserve(data.size() + prefix.size() * 2);
        for (const char c : data) {
            if (m_atLineStart) {
                out.append(prefix);
                m_atLineStart = false;
            }
            out.append(c);
            if (c == '\n') {
                m_atLineStart = true;
            }
        }
        writeRaw(out);
        break;
    }
    case Format::HexDump: {
        QString block = QStringLiteral("[%1] RX (%2 bytes)\n").arg(timestamp()).arg(data.size());
        block += HexUtils::hexDump(data);
        block += QLatin1Char('\n');
        writeRaw(block.toUtf8());
        break;
    }
    }
}

void SessionLogger::logSent(const QByteArray& data)
{
    if (!isActive() || data.isEmpty() || !m_includeTx || m_format == Format::Raw) {
        return;
    }
    if (m_format == Format::Text) {
        QString line;
        if (!m_atLineStart) {
            line += QLatin1Char('\n');   // TX goes on its own line
        }
        line += QStringLiteral("[%1] TX> %2\n").arg(timestamp(), HexUtils::escapeForDisplay(data));
        m_atLineStart = true;
        writeRaw(line.toUtf8());
        return;
    }
    QString block = QStringLiteral("[%1] TX (%2 bytes)\n").arg(timestamp()).arg(data.size());
    block += HexUtils::hexDump(data);
    block += QLatin1Char('\n');
    writeRaw(block.toUtf8());
}

void SessionLogger::writeRaw(const QByteArray& bytes)
{
    if (!m_file || bytes.isEmpty()) {
        return;
    }
    const qint64 written = m_file->write(bytes);
    if (written < 0) {
        const QString message = tr("Write to log file %1 failed: %2")
                                    .arg(QDir::toNativeSeparators(m_file->fileName()), m_file->errorString());
        qCWarning(lcSerial) << message;
        stop();
        emit error(message);
        return;
    }
    // Survive a crash; no fsync (see DESIGN.md 4.6). Qt buffers small writes, so a full disk or
    // read-only share surfaces here rather than from write().
    if (!m_file->flush()) {
        const QString message = tr("Write to log file %1 failed: %2")
                                    .arg(QDir::toNativeSeparators(m_file->fileName()), m_file->errorString());
        qCWarning(lcSerial) << message;
        stop();
        emit error(message);
        return;
    }
    m_bytesWritten += written;   // only count data that reached the OS
}

QString SessionLogger::timestamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}
