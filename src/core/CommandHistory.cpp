#include "core/CommandHistory.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>

CommandHistory::CommandHistory(int maxEntries)
    : m_maxEntries(qMax(1, maxEntries))
    , m_cursor(0)
{
}

void CommandHistory::add(const QString& command)
{
    if (command.trimmed().isEmpty()) {
        resetNavigation();
        return;
    }
    if (m_entries.isEmpty() || m_entries.last() != command) {
        m_entries.append(command);
        while (m_entries.size() > m_maxEntries) {
            m_entries.removeFirst();
        }
    }
    resetNavigation();
}

QString CommandHistory::previous()
{
    if (m_entries.isEmpty()) {
        return QString();
    }
    if (m_cursor > 0) {
        --m_cursor;
    }
    return m_entries.at(m_cursor);
}

QString CommandHistory::next()
{
    if (m_entries.isEmpty()) {
        m_cursor = 0;
        return QString();
    }
    if (m_cursor < m_entries.size()) {
        ++m_cursor;
    }
    if (m_cursor >= m_entries.size()) {
        m_cursor = static_cast<int>(m_entries.size());
        return QString();
    }
    return m_entries.at(m_cursor);
}

void CommandHistory::resetNavigation()
{
    m_cursor = static_cast<int>(m_entries.size());
}

bool CommandHistory::isNavigating() const
{
    return m_cursor < m_entries.size();
}

QStringList CommandHistory::entries() const
{
    return m_entries;
}

void CommandHistory::setEntries(const QStringList& entries)
{
    m_entries.clear();
    for (const QString& entry : entries) {
        if (entry.trimmed().isEmpty()) {
            continue;
        }
        m_entries.append(entry);
    }
    while (m_entries.size() > m_maxEntries) {
        m_entries.removeFirst();
    }
    resetNavigation();
}

void CommandHistory::clear()
{
    m_entries.clear();
    resetNavigation();
}

int CommandHistory::size() const
{
    return static_cast<int>(m_entries.size());
}

int CommandHistory::maxEntries() const
{
    return m_maxEntries;
}

void CommandHistory::setMaxEntries(int max)
{
    m_maxEntries = qMax(1, max);
    if (m_entries.size() > m_maxEntries) {
        m_entries.erase(m_entries.begin(), m_entries.begin() + (m_entries.size() - m_maxEntries));
        resetNavigation();
    } else if (m_cursor > m_entries.size()) {
        resetNavigation();
    }
}

bool CommandHistory::load(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    QStringList lines;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (!line.trimmed().isEmpty()) {
            lines.append(line);
        }
    }
    setEntries(lines);
    return true;
}

bool CommandHistory::save(const QString& filePath) const
{
    const QFileInfo info(filePath);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath())) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    for (const QString& entry : m_entries) {
        out << entry << '\n';
    }
    out.flush();
    return out.status() == QTextStream::Ok && file.error() == QFile::NoError;
}
