#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <cstdint>

/**
 * Plain data types shared by TerminalScreen (model), AnsiParser (producer) and
 * TerminalWidget (renderer). No Qt widgets here - this header is used by the unit tests.
 */
namespace Terminal {

/// A colour as specified by SGR: default, one of 256 indexed colours, or 24-bit RGB.
struct Color
{
    enum Kind : quint8 { Default = 0, Indexed = 1, Rgb = 2 };
    Kind kind = Default;
    quint8 index = 0;      ///< 0..255 when kind == Indexed (0-7 normal, 8-15 bright, 16-231 cube, 232-255 grey)
    quint8 r = 0, g = 0, b = 0;

    static Color indexed(int i) { Color c; c.kind = Indexed; c.index = static_cast<quint8>(i & 0xFF); return c; }
    static Color rgb(int r, int g, int b) { Color c; c.kind = Rgb; c.r = quint8(r); c.g = quint8(g); c.b = quint8(b); return c; }
    bool isDefault() const { return kind == Default; }
    bool operator==(const Color& o) const { return kind == o.kind && index == o.index && r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

/// SGR attribute flags.
enum AttrFlag : quint16 {
    NoAttr    = 0,
    Bold      = 1 << 0,
    Dim       = 1 << 1,
    Italic    = 1 << 2,
    Underline = 1 << 3,
    Blink     = 1 << 4,
    Inverse   = 1 << 5,
    Hidden    = 1 << 6,
    Strike    = 1 << 7,
    /// Cell holds the first half of a double-width (CJK) character.
    WideLead  = 1 << 8,
    /// Cell is the placeholder second half of a double-width character (ch == 0).
    WideTrail = 1 << 9,
};

struct Attributes
{
    Color fg;
    Color bg;
    quint16 flags = NoAttr;

    bool has(AttrFlag f) const { return (flags & f) != 0; }
    void set(AttrFlag f, bool on) { if (on) flags |= f; else flags &= static_cast<quint16>(~f); }
    bool operator==(const Attributes& o) const { return fg == o.fg && bg == o.bg && flags == o.flags; }
    bool operator!=(const Attributes& o) const { return !(*this == o); }
};

/// One character cell. `ch` is a Unicode code point (U+0020 for blank, 0 for a wide trail).
struct Cell
{
    char32_t ch = U' ';
    Attributes attr;

    bool isBlank() const { return ch == U' ' || ch == 0; }
    bool isWideTrail() const { return attr.has(WideTrail); }
    bool operator==(const Cell& o) const { return ch == o.ch && attr == o.attr; }
};

/// One row of the screen or scrollback.
struct Line
{
    QVector<Cell> cells;
    /// True when this line was soft-wrapped by autowrap and logically continues on the
    /// next line (used to join lines when copying text and to reflow on resize).
    bool wrapped = false;

    int length() const { return cells.size(); }
    /// Text of the line with trailing blanks trimmed; wide trail cells contribute nothing.
    QString text(bool trimRight = true) const;
};

struct Cursor
{
    int row = 0;
    int col = 0;
    bool visible = true;
    bool operator==(const Cursor& o) const { return row == o.row && col == o.col && visible == o.visible; }
};

/// Concrete colours used by the renderer for a theme.
struct Palette
{
    QString name;
    QColor ansi[16];          ///< 0-7 normal, 8-15 bright
    QColor foreground;        ///< default text colour
    QColor background;        ///< default background colour
    QColor cursor;
    QColor cursorText;
    QColor selection;         ///< selection background (alpha allowed)
    QColor selectionText;

    /// Resolve a Color to a QColor. `isForeground` selects the default; when `bold` is set
    /// and the colour is an indexed 0-7 colour, the bright variant (8-15) is used
    /// ("bold is bright", the behaviour most users expect from boot logs).
    QColor resolve(const Color& color, bool isForeground, bool bold = false) const;
    /// The xterm 256-colour value for index 16..255 (cube + greys); 0..15 -> ansi[].
    QColor indexed(int index) const;
};

} // namespace Terminal
