#include "core/HexUtils.h"

#include <QStringList>

namespace HexUtils {

namespace {

constexpr char kUpperDigits[] = "0123456789ABCDEF";
constexpr char kLowerDigits[] = "0123456789abcdef";

int hexValue(QChar c)
{
    const ushort u = c.unicode();
    if (u >= '0' && u <= '9') {
        return u - '0';
    }
    if (u >= 'a' && u <= 'f') {
        return u - 'a' + 10;
    }
    if (u >= 'A' && u <= 'F') {
        return u - 'A' + 10;
    }
    return -1;
}

bool isSeparator(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char(',') || c == QLatin1Char(':') ||
           c == QLatin1Char('-') || c == QLatin1Char('\n') || c == QLatin1Char('\r');
}

void appendHexByte(QString& out, quint8 byte, const char* digits)
{
    out.append(QLatin1Char(digits[byte >> 4]));
    out.append(QLatin1Char(digits[byte & 0x0F]));
}

/// Length of the valid UTF-8 sequence starting at `pos`, or 0 when the bytes at `pos` do
/// not start a well-formed multi-byte sequence.
int validUtf8SequenceLength(const QByteArray& data, qsizetype pos)
{
    const auto lead = static_cast<quint8>(data.at(pos));
    int needed = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        needed = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        needed = 2;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        needed = 3;
    } else {
        return 0;
    }
    if (pos + needed >= data.size()) {
        return 0;
    }
    for (int i = 1; i <= needed; ++i) {
        const auto cont = static_cast<quint8>(data.at(pos + i));
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
    }
    // Reject overlong forms, surrogates and out-of-range code points: Qt's decoder maps
    // those to U+FFFD, which a well-formed sequence never produces.
    const QString decoded = QString::fromUtf8(data.constData() + pos, needed + 1);
    if (decoded.contains(QChar(0xFFFD))) {
        return 0;
    }
    return needed + 1;
}

} // namespace

bool parseHexString(const QString& text, QByteArray& out, QString* error)
{
    out.clear();
    if (error) {
        error->clear();
    }

    int pendingHigh = -1;   // high nibble waiting for its partner, -1 when at a pair boundary
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = text.at(i);
        if (isSeparator(c)) {
            ++i;
            continue;
        }
        // Optional "0x" / "0X" prefix, only recognised at a pair boundary.
        if (pendingHigh < 0 && c == QLatin1Char('0') && i + 1 < n &&
            (text.at(i + 1) == QLatin1Char('x') || text.at(i + 1) == QLatin1Char('X'))) {
            i += 2;
            continue;
        }
        const int v = hexValue(c);
        if (v < 0) {
            if (error) {
                *error = QStringLiteral("Invalid character '%1' at position %2").arg(c).arg(i + 1);
            }
            out.clear();
            return false;
        }
        if (pendingHigh < 0) {
            pendingHigh = v;
        } else {
            out.append(static_cast<char>((pendingHigh << 4) | v));
            pendingHigh = -1;
        }
        ++i;
    }
    if (pendingHigh >= 0) {
        if (error) {
            *error = QStringLiteral("Odd number of hex digits (missing nibble at position %1)").arg(n);
        }
        out.clear();
        return false;
    }
    return true;
}

QString toHexString(const QByteArray& data, const QString& separator, bool upperCase)
{
    const char* digits = upperCase ? kUpperDigits : kLowerDigits;
    QString result;
    result.reserve(data.size() * (2 + separator.size()));
    for (qsizetype i = 0; i < data.size(); ++i) {
        if (i > 0) {
            result.append(separator);
        }
        appendHexByte(result, static_cast<quint8>(data.at(i)), digits);
    }
    return result;
}

QString hexDump(const QByteArray& data, qint64 baseOffset, int bytesPerLine)
{
    if (data.isEmpty()) {
        return QString();
    }
    if (bytesPerLine <= 0) {
        bytesPerLine = 16;
    }
    // Width of the hex column for a full line: "XX" per byte, one space between bytes and
    // one extra space after every 8th byte.
    const int fullHexWidth = bytesPerLine * 3 - 1 + (bytesPerLine - 1) / 8;

    QStringList lines;
    for (qsizetype start = 0; start < data.size(); start += bytesPerLine) {
        const qsizetype count = qMin<qsizetype>(bytesPerLine, data.size() - start);
        QString line;
        line.reserve(10 + fullHexWidth + 4 + bytesPerLine);

        const qint64 offset = baseOffset + static_cast<qint64>(start);
        line.append(QStringLiteral("%1").arg(static_cast<quint64>(offset), 8, 16, QLatin1Char('0')).toUpper());
        line.append(QStringLiteral("  "));

        QString hex;
        for (qsizetype i = 0; i < count; ++i) {
            if (i > 0) {
                hex.append(QLatin1Char(' '));
                if (i % 8 == 0) {
                    hex.append(QLatin1Char(' '));
                }
            }
            appendHexByte(hex, static_cast<quint8>(data.at(start + i)), kUpperDigits);
        }
        line.append(hex);
        line.append(QString(fullHexWidth - hex.size() + 2, QLatin1Char(' ')));
        line.append(QLatin1Char('|'));
        line.append(printableAscii(data.mid(start, count)));
        line.append(QLatin1Char('|'));
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

QString printableAscii(const QByteArray& data)
{
    QString result;
    result.reserve(data.size());
    for (const char ch : data) {
        const auto b = static_cast<quint8>(ch);
        result.append((b >= 0x20 && b <= 0x7E) ? QLatin1Char(ch) : QLatin1Char('.'));
    }
    return result;
}

QByteArray unescape(const QString& text, QString* error)
{
    QByteArray out;
    QString pending;   // decoded text not yet converted to UTF-8 (so surrogate pairs stay intact)

    auto flush = [&out, &pending]() {
        if (!pending.isEmpty()) {
            out.append(pending.toUtf8());
            pending.clear();
        }
    };
    auto fail = [&out, error](const QString& message) {
        if (error) {
            *error = message;
        }
        out.clear();
        return QByteArray();
    };

    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('\\')) {
            pending.append(c);
            ++i;
            continue;
        }
        if (i + 1 >= n) {
            return fail(QStringLiteral("Trailing backslash at position %1").arg(i + 1));
        }
        const QChar e = text.at(i + 1);
        switch (e.unicode()) {
        case 'n':
            pending.append(QLatin1Char('\n'));
            i += 2;
            break;
        case 'r':
            pending.append(QLatin1Char('\r'));
            i += 2;
            break;
        case 't':
            pending.append(QLatin1Char('\t'));
            i += 2;
            break;
        case '0':
            pending.append(QChar(0));
            i += 2;
            break;
        case 'a':
            pending.append(QChar(0x07));
            i += 2;
            break;
        case 'b':
            pending.append(QChar(0x08));
            i += 2;
            break;
        case 'e':
            pending.append(QChar(0x1B));
            i += 2;
            break;
        case 'f':
            pending.append(QChar(0x0C));
            i += 2;
            break;
        case 'v':
            pending.append(QChar(0x0B));
            i += 2;
            break;
        case '\\':
            pending.append(QLatin1Char('\\'));
            i += 2;
            break;
        case 'x': {
            if (i + 4 > n) {   // fewer than two characters follow "\x"
                return fail(QStringLiteral("\\x needs exactly two hex digits at position %1").arg(i + 1));
            }
            const int hi = hexValue(text.at(i + 2));
            const int lo = hexValue(text.at(i + 3));
            if (hi < 0 || lo < 0) {
                return fail(QStringLiteral("\\x needs exactly two hex digits at position %1").arg(i + 1));
            }
            flush();
            out.append(static_cast<char>((hi << 4) | lo));
            i += 4;
            break;
        }
        case 'u': {
            if (i + 6 > n) {   // fewer than four characters follow "\u"
                return fail(QStringLiteral("\\u needs exactly four hex digits at position %1").arg(i + 1));
            }
            int value = 0;
            for (qsizetype k = 0; k < 4; ++k) {
                const int v = hexValue(text.at(i + 2 + k));
                if (v < 0) {
                    return fail(QStringLiteral("\\u needs exactly four hex digits at position %1").arg(i + 1));
                }
                value = (value << 4) | v;
            }
            pending.append(QChar(static_cast<ushort>(value)));
            i += 6;
            break;
        }
        default:
            return fail(QStringLiteral("Unknown escape sequence '\\%1' at position %2").arg(e).arg(i + 1));
        }
    }
    flush();
    if (error) {
        error->clear();
    }
    return out;
}

QString escapeForDisplay(const QByteArray& data)
{
    QString result;
    result.reserve(data.size());
    qsizetype i = 0;
    while (i < data.size()) {
        const auto b = static_cast<quint8>(data.at(i));
        if (b >= 0x20 && b <= 0x7E) {
            result.append(QLatin1Char(static_cast<char>(b)));
            ++i;
        } else if (b == '\r') {
            result.append(QStringLiteral("\\r"));
            ++i;
        } else if (b == '\n') {
            result.append(QStringLiteral("\\n"));
            ++i;
        } else if (b == '\t') {
            result.append(QStringLiteral("\\t"));
            ++i;
        } else if (b >= 0x80) {
            const int len = validUtf8SequenceLength(data, i);
            if (len > 0) {
                result.append(QString::fromUtf8(data.constData() + i, len));
                i += len;
            } else {
                result.append(QStringLiteral("\\x"));
                appendHexByte(result, b, kUpperDigits);
                ++i;
            }
        } else {
            result.append(QStringLiteral("\\x"));
            appendHexByte(result, b, kUpperDigits);
            ++i;
        }
    }
    return result;
}

} // namespace HexUtils
