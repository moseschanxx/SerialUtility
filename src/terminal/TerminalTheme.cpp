#include "terminal/TerminalTheme.h"

#include <QCoreApplication>

namespace {

const char* const kThemeDark = "dark";
const char* const kThemeLight = "light";
const char* const kThemeSolarizedDark = "solarized-dark";
const char* const kThemeMatrix = "matrix";

void setAnsi(Terminal::Palette& p, const char* const (&colors)[16])
{
    for (int i = 0; i < 16; ++i) {
        p.ansi[i] = QColor(QLatin1StringView(colors[i]));
    }
}

} // namespace

// ---- Terminal::Palette ------------------------------------------------------------------

namespace Terminal {

QColor Palette::indexed(int index) const
{
    if (index < 0) {
        index = 0;
    } else if (index > 255) {
        index = 255;
    }
    if (index < 16) {
        return ansi[index];
    }
    if (index < 232) {
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        const int i = index - 16;
        return QColor(levels[i / 36], levels[(i / 6) % 6], levels[i % 6]);
    }
    const int grey = 8 + 10 * (index - 232);
    return QColor(grey, grey, grey);
}

QColor Palette::resolve(const Color& color, bool isForeground, bool bold) const
{
    switch (color.kind) {
    case Color::Indexed: {
        int idx = color.index;
        if (bold && isForeground && idx < 8) {
            idx += 8;
        }
        return indexed(idx);
    }
    case Color::Rgb:
        return QColor(color.r, color.g, color.b);
    case Color::Default:
    default:
        return isForeground ? foreground : background;
    }
}

} // namespace Terminal

// ---- TerminalTheme ----------------------------------------------------------------------

QStringList TerminalTheme::names()
{
    return {QLatin1StringView(kThemeDark), QLatin1StringView(kThemeLight), QLatin1StringView(kThemeSolarizedDark),
            QLatin1StringView(kThemeMatrix)};
}

QString TerminalTheme::displayName(const QString& name)
{
    if (name == QLatin1StringView(kThemeDark)) {
        return QCoreApplication::translate("TerminalTheme", "Dark");
    }
    if (name == QLatin1StringView(kThemeLight)) {
        return QCoreApplication::translate("TerminalTheme", "Light");
    }
    if (name == QLatin1StringView(kThemeSolarizedDark)) {
        return QCoreApplication::translate("TerminalTheme", "Solarized Dark");
    }
    if (name == QLatin1StringView(kThemeMatrix)) {
        return QCoreApplication::translate("TerminalTheme", "Matrix");
    }
    return name;
}

Terminal::Palette TerminalTheme::palette(const QString& name)
{
    if (name == QLatin1StringView(kThemeLight)) {
        return light();
    }
    if (name == QLatin1StringView(kThemeSolarizedDark)) {
        return solarizedDark();
    }
    if (name == QLatin1StringView(kThemeMatrix)) {
        return matrix();
    }
    return dark();
}

Terminal::Palette TerminalTheme::dark()
{
    Terminal::Palette p;
    p.name = QLatin1StringView(kThemeDark);
    static const char* const colors[16] = {
        "#000000", "#CD3131", "#0DBC79", "#E5E510", "#2472C8", "#BC3FBC", "#11A8CD", "#E5E5E5", // normal
        "#666666", "#F14C4C", "#23D18B", "#F5F543", "#3B8EEA", "#D670D6", "#29B8DB", "#FFFFFF", // bright
    };
    setAnsi(p, colors);
    p.foreground = QColor(0xD4, 0xD4, 0xD4);
    p.background = QColor(0x1E, 0x1E, 0x1E);
    p.cursor = QColor(0xAE, 0xAF, 0xAD);
    p.cursorText = QColor(0x1E, 0x1E, 0x1E);
    p.selection = QColor(0x26, 0x4F, 0x78, 0xC0);
    p.selectionText = QColor(0xFF, 0xFF, 0xFF);
    return p;
}

Terminal::Palette TerminalTheme::light()
{
    Terminal::Palette p;
    p.name = QLatin1StringView(kThemeLight);
    static const char* const colors[16] = {
        "#000000", "#CD3131", "#00BC00", "#949800", "#0451A5", "#BC05BC", "#0598BC", "#555555", // normal
        "#666666", "#CD3131", "#14CE14", "#B5BA00", "#0451A5", "#BC05BC", "#0598BC", "#A5A5A5", // bright
    };
    setAnsi(p, colors);
    p.foreground = QColor(0x38, 0x3A, 0x42);
    p.background = QColor(0xFF, 0xFF, 0xFF);
    p.cursor = QColor(0x38, 0x3A, 0x42);
    p.cursorText = QColor(0xFF, 0xFF, 0xFF);
    p.selection = QColor(0xAD, 0xD6, 0xFF, 0xC0);
    p.selectionText = QColor(0x00, 0x00, 0x00);
    return p;
}

Terminal::Palette TerminalTheme::solarizedDark()
{
    Terminal::Palette p;
    p.name = QLatin1StringView(kThemeSolarizedDark);
    static const char* const colors[16] = {
        "#073642", "#DC322F", "#859900", "#B58900", "#268BD2", "#D33682", "#2AA198", "#EEE8D5", // normal
        "#002B36", "#CB4B16", "#586E75", "#657B83", "#839496", "#6C71C4", "#93A1A1", "#FDF6E3", // bright
    };
    setAnsi(p, colors);
    p.foreground = QColor(0x83, 0x94, 0x96);
    p.background = QColor(0x00, 0x2B, 0x36);
    p.cursor = QColor(0x83, 0x94, 0x96);
    p.cursorText = QColor(0x00, 0x2B, 0x36);
    p.selection = QColor(0x07, 0x36, 0x42, 0xE0);
    p.selectionText = QColor(0x93, 0xA1, 0xA1);
    return p;
}

Terminal::Palette TerminalTheme::matrix()
{
    Terminal::Palette p;
    p.name = QLatin1StringView(kThemeMatrix);
    static const char* const colors[16] = {
        "#000000", "#007A1F", "#00A82A", "#00C832", "#005F17", "#009924", "#00D437", "#00FF41", // normal
        "#003B0F", "#1AFF5C", "#33FF70", "#66FF8F", "#0DAA33", "#4DFF80", "#80FFA6", "#B3FFC9", // bright
    };
    setAnsi(p, colors);
    p.foreground = QColor(0x00, 0xFF, 0x41);
    p.background = QColor(0x00, 0x00, 0x00);
    p.cursor = QColor(0x00, 0xFF, 0x41);
    p.cursorText = QColor(0x00, 0x00, 0x00);
    p.selection = QColor(0x00, 0xFF, 0x41, 0x60);
    p.selectionText = QColor(0xFF, 0xFF, 0xFF);
    return p;
}
