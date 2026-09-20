#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

/**
 * Byte <-> text helpers used by the command input (hex send), the hex-dump view and the
 * session logger. Pure functions, fully unit-tested (tests/tst_hexutils.cpp).
 */
namespace HexUtils {

/**
 * Parse a user-typed hex string into bytes.
 * Accepted input: hex digit pairs separated by any mix of spaces, commas, colons, dashes
 * or nothing, each pair optionally prefixed with "0x"/"0X". Case-insensitive.
 *   "AA BB cc" -> AA BB CC       "0xAA,0x0d" -> AA 0D       "aabb" -> AA BB
 * Returns false (and sets *error, if given, to a translatable-free English description
 * with the offending position) on an odd number of nibbles or an invalid character.
 * An empty / whitespace-only string yields an empty array and returns true.
 */
bool parseHexString(const QString& text, QByteArray& out, QString* error = nullptr);

/// "48 65 6C 6C 6F" (upper) or "48 65 6c 6c 6f" (lower). Empty separator allowed.
QString toHexString(const QByteArray& data, const QString& separator = QStringLiteral(" "), bool upperCase = true);

/**
 * Classic hex dump, one or more lines:
 *   "00000000  48 65 6C 6C 6F 20 57 6F  72 6C 64 0D 0A           |Hello World..|"
 * - 8-digit upper-case hex offset (baseOffset + line start), two spaces
 * - bytesPerLine bytes as 2-digit upper hex separated by single spaces, an extra space
 *   after the 8th byte, padded so the ASCII column always aligns
 * - ASCII column wrapped in '|' with non-printables shown as '.'
 * Lines are joined with '\n'; no trailing newline. Empty input -> empty string.
 */
QString hexDump(const QByteArray& data, qint64 baseOffset = 0, int bytesPerLine = 16);

/// Printable ASCII (0x20..0x7E) preserved, everything else replaced by '.'.
QString printableAscii(const QByteArray& data);

/**
 * Interpret C-style escapes in `text` and encode the result as UTF-8:
 *   \n \r \t \0 \a \b \e (ESC) \f \v \\ \xHH (exactly two hex digits) \uHHHH
 * Any other backslash sequence is an error (returns empty array, sets *error).
 * `ok` semantics: on success *error (if given) is cleared.
 */
QByteArray unescape(const QString& text, QString* error = nullptr);

/**
 * Same parsing as unescape(text, error), but the caller chooses how *text* becomes bytes:
 * every run of literal characters and simple escapes (\n \r \t \0 \a \b \e \f \v \\ and
 * \uHHHH, surrogate pairs kept intact) is passed to `encodeText` when it is flushed, while
 * every \xHH is appended as exactly that byte and never goes through `encodeText`. This lets
 * a session in a non-UTF-8 encoding transcode the text while "\xHH = this exact byte" holds.
 * unescape(text, error) is this overload with `[](const QString& s) { return s.toUtf8(); }`.
 */
QByteArray unescape(const QString& text, const std::function<QByteArray(const QString&)>& encodeText,
                    QString* error = nullptr);

/// Inverse of unescape for display: control bytes become \xHH (or \r \n \t), printable
/// ASCII stays, bytes >= 0x80 are decoded as UTF-8 when valid otherwise \xHH.
QString escapeForDisplay(const QByteArray& data);

} // namespace HexUtils
