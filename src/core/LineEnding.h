#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

/**
 * Line-ending handling shared by the terminal (Enter key), the command input box,
 * quick commands and the file sender.
 */
namespace LineEnding {

enum class Mode {
    None = 0,   ///< send exactly what was typed
    CR,         ///< "\r"   - what a tty / getty / U-Boot expects from a terminal (default)
    LF,         ///< "\n"
    CRLF        ///< "\r\n" - many MCU AT-style shells
};

/// Bytes appended for the given mode ("" for None).
QByteArray bytes(Mode mode);

/// Human readable label for combo boxes, e.g. "CR (\\r)". Not translated (technical).
QString displayName(Mode mode);

/// All modes in UI order: None, CR, LF, CRLF.
QList<Mode> allModes();

/// Stable settings key: "none" | "cr" | "lf" | "crlf". Unknown -> CR.
QString toString(Mode mode);
Mode fromString(const QString& key);

} // namespace LineEnding
