#include "core/LineEnding.h"

namespace LineEnding {

QByteArray bytes(Mode mode)
{
    switch (mode) {
    case Mode::CR:
        return QByteArrayLiteral("\r");
    case Mode::LF:
        return QByteArrayLiteral("\n");
    case Mode::CRLF:
        return QByteArrayLiteral("\r\n");
    case Mode::None:
        break;
    }
    return QByteArray();
}

QString displayName(Mode mode)
{
    switch (mode) {
    case Mode::CR:
        return QStringLiteral("CR (\\r)");
    case Mode::LF:
        return QStringLiteral("LF (\\n)");
    case Mode::CRLF:
        return QStringLiteral("CR+LF (\\r\\n)");
    case Mode::None:
        break;
    }
    return QStringLiteral("None");
}

QList<Mode> allModes()
{
    return {Mode::None, Mode::CR, Mode::LF, Mode::CRLF};
}

QString toString(Mode mode)
{
    switch (mode) {
    case Mode::None:
        return QStringLiteral("none");
    case Mode::CR:
        return QStringLiteral("cr");
    case Mode::LF:
        return QStringLiteral("lf");
    case Mode::CRLF:
        return QStringLiteral("crlf");
    }
    return QStringLiteral("cr");
}

Mode fromString(const QString& key)
{
    const QString k = key.trimmed().toLower();
    if (k == QLatin1String("none")) {
        return Mode::None;
    }
    if (k == QLatin1String("lf")) {
        return Mode::LF;
    }
    if (k == QLatin1String("crlf")) {
        return Mode::CRLF;
    }
    return Mode::CR;
}

} // namespace LineEnding
