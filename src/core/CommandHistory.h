#pragma once

#include <QString>
#include <QStringList>

/**
 * Shell-like command history for the line-mode command input.
 *
 * Navigation model (like bash): the history is a list, oldest first. A cursor starts
 * "below" the newest entry (index == size()). previous() moves the cursor up and returns
 * that entry; next() moves it down; moving below the newest entry returns an empty string
 * (the user's blank line). add() appends and resets the cursor.
 *
 * Not a QObject: plain value class, copyable, unit-tested (tests/tst_commandhistory.cpp).
 */
class CommandHistory
{
public:
    explicit CommandHistory(int maxEntries = 500);

    /// Append `command`. Empty/whitespace-only strings are ignored. If the command equals
    /// the newest entry it is not duplicated. Oldest entries are dropped beyond maxEntries.
    /// Always resets navigation.
    void add(const QString& command);

    /// Move up (older). Returns the entry at the new position, or the oldest entry again
    /// when already at the top. Returns an empty string if the history is empty.
    QString previous();

    /// Move down (newer). Returns the entry at the new position, or an empty string once
    /// the cursor passes the newest entry (back to the blank line).
    QString next();

    /// Put the cursor back below the newest entry.
    void resetNavigation();

    /// True while the cursor points at an entry (i.e. after previous() without returning
    /// to the blank line).
    bool isNavigating() const;

    QStringList entries() const;
    void setEntries(const QStringList& entries);   ///< trims to maxEntries, resets navigation
    void clear();
    int size() const;
    int maxEntries() const;
    void setMaxEntries(int max);                   ///< drops oldest entries if needed

    /// Plain-text persistence: one command per line, UTF-8. Returns false on I/O error.
    bool load(const QString& filePath);
    bool save(const QString& filePath) const;

private:
    QStringList m_entries;
    int m_maxEntries;
    int m_cursor;   ///< 0..size(); size() means "blank line below newest"
};
