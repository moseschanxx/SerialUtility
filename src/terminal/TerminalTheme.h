#pragma once

#include <QString>
#include <QStringList>

#include "terminal/TerminalTypes.h"

/**
 * Built-in colour themes for the terminal.
 *
 *  "dark"  (default) - VS Code "Dark+"-like: background #1E1E1E, foreground #D4D4D4,
 *                      ANSI: black #000000, red #CD3131, green #0DBC79, yellow #E5E510,
 *                      blue #2472C8, magenta #BC3FBC, cyan #11A8CD, white #E5E5E5,
 *                      bright: #666666 #F14C4C #23D18B #F5F543 #3B8EEA #D670D6 #29B8DB #FFFFFF
 *  "light"           - background #FFFFFF, foreground #383A42, standard light ANSI set
 *  "solarized-dark"  - Solarized Dark
 *  "matrix"          - black background, green text (for fun / high contrast)
 */
class TerminalTheme
{
public:
    static QStringList names();                          ///< in UI order, "dark" first
    static QString displayName(const QString& name);     ///< translated label
    static Terminal::Palette palette(const QString& name);   ///< unknown -> dark()
    static Terminal::Palette dark();
    static Terminal::Palette light();
    static Terminal::Palette solarizedDark();
    static Terminal::Palette matrix();
};
