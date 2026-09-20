#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>

#include "core/LineEnding.h"

/**
 * One user-defined macro shown as a button in the QuickCommandBar.
 */
struct QuickCommand
{
    QString name;                                       ///< button label, e.g. "uname -a"
    QString command;                                    ///< text to send, or hex string when `hex` is true
    QString group;                                      ///< e.g. "Linux", "U-Boot", "MCU"; empty/whitespace = the
                                                        ///< general group (QuickCommandStore::generalGroupKey())
    LineEnding::Mode lineEnding = LineEnding::Mode::CR; ///< appended after `command` (ignored when hex)
    bool hex = false;                                   ///< interpret `command` with HexUtils::parseHexString
    bool escapes = false;                               ///< interpret C-style escapes (HexUtils::unescape); ignored when hex
    QString shortcut;                                   ///< optional QKeySequence::toString() portable text, e.g. "Ctrl+1"
    QString tooltip;                                    ///< optional description

    /// Resolve the bytes to transmit: hex -> parsed bytes (no line ending);
    /// otherwise UTF-8 of command (after unescape when `escapes`) + LineEnding::bytes(lineEnding).
    /// On a parse error returns an empty array and sets *error.
    QByteArray payload(QString* error = nullptr) const;

    QJsonObject toJson() const;
    static QuickCommand fromJson(const QJsonObject& obj);   ///< missing fields -> defaults above

    bool operator==(const QuickCommand& other) const;
    bool operator!=(const QuickCommand& other) const { return !(*this == other); }
};

/**
 * Owns the list of quick commands and persists it as JSON:
 *   { "version": 1, "commands": [ {...}, ... ] }
 * at AppSettings::dataDirectory()/quick_commands.json. If the file does not exist the
 * built-in defaults() are used (and saved on first save()).
 */
class QuickCommandStore : public QObject
{
    Q_OBJECT
public:
    explicit QuickCommandStore(QObject* parent = nullptr);

    static QString defaultFilePath();

    /// Load from `path` (default: defaultFilePath()). Missing file -> defaults(), returns true.
    /// Malformed JSON -> keeps current list, returns false.
    bool load(const QString& path = QString());
    /// Save to `path` (default: defaultFilePath()); creates the directory. Returns false on I/O error.
    bool save(const QString& path = QString()) const;

    QList<QuickCommand> commands() const;
    void setCommands(const QList<QuickCommand>& commands);   ///< emits changed() if different
    /// Distinct effective groups in first-seen order; empty/whitespace group -> generalGroupKey() (untranslated).
    QStringList groups() const;

    /// Untranslated canonical key ("General") used in groups(), in the QuickCommandBar's combo item data and in
    /// QSettings "ui/quickCommandGroup"; never shown directly, see groupDisplayName().
    static QString generalGroupKey();
    /// `command.group` trimmed, or generalGroupKey() when that is empty.
    static QString effectiveGroup(const QuickCommand& command);
    /// Text to show for a group key: tr("General") for generalGroupKey(), the group itself otherwise.
    static QString groupDisplayName(const QString& group);

    /// Sensible starter set (see docs/DESIGN.md "Default quick commands"):
    /// Linux: uname -a, cat /proc/cpuinfo, cat /proc/meminfo, df -h, ifconfig, dmesg | tail -n 50, ps, top -n 1
    /// U-Boot: help, printenv, bdinfo, version, mmc info, boot
    /// MCU: help, version, AT (CRLF), reset
    /// Control: Ctrl+C (hex "03"), Ctrl+D (hex "04"), Ctrl+Z (hex "1A"), ESC (hex "1B")
    static QList<QuickCommand> defaults();
    void resetToDefaults();

    /// Import/export in the same JSON format (for sharing between machines).
    bool importFromFile(const QString& path, QString* error = nullptr);
    bool exportToFile(const QString& path, QString* error = nullptr) const;

signals:
    void changed();

private:
    QList<QuickCommand> m_commands;
};
