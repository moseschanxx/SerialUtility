#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringDecoder>
#include <QVector>
#include <memory>

class TerminalScreen;
namespace Terminal {
struct Color;
}

/**
 * Decodes incoming bytes (stateful text decoder, default UTF-8) and interprets C0
 * controls, ESC sequences, CSI, OSC, DCS/SOS/PM/APC strings, applying them to a
 * TerminalScreen. Implements the state machine of Paul Williams' VT500-series parser
 * (https://vt100.net/emu/dec_ansi_parser) so malformed input never desynchronises the
 * output: unknown sequences are consumed and ignored (logged at debug level on lcTerminal).
 * Non-OSC control strings abort on a non-ASCII code point or after 4096 code points so line
 * noise cannot silence the console.
 *
 * Supported (see docs/TERMINAL_EMULATION.md for the full table):
 *  C0:  BEL BS HT LF VT FF CR SO/SI (select G1/G0) ESC; other C0 ignored. DEL ignored.
 *  ESC: 7 (DECSC, also saves the charset state) 8 (DECRC) D (IND) E (NEL) H (HTS) M (RI)
 *       c (RIS) = > (keypad, ignored), ( ) <charset> (B = ASCII, 0 = DEC Special Graphics;
 *       G2/G3 and 96-sets ignored), # 8 (DECALN: fill screen with 'E')
 *  CSI: @ ICH, A CUU, B CUD, C CUF, D CUB, E CNL, F CPL, G CHA, H CUP, I CHT, J ED, K EL,
 *       L IL, M DL, P DCH, S SU, T SD, X ECH, Z CBT, ` HPA, a HPR, b REP, c DA (reply
 *       "\e[?1;2c"), d VPA, e VPR, f HVP, g TBC, h/l SM/RM (4 IRM, 20 LNM),
 *       ?h/?l DECSET/DECRST (1 DECCKM cursor-key mode -> cursorKeyModeChanged, 6 DECOM, 7 DECAWM,
 *       12 cursor blink ignored, 25 DECTCEM, 47/1047/1049 alternate screen, 2004 bracketed
 *       paste -> bracketedPasteChanged, others ignored), m SGR, n DSR (5 -> "\e[0n",
 *       6 -> "\e[<row>;<col>R" 1-based), r DECSTBM, s SCOSC, u SCORC, t (window ops ignored),
 *       ! p DECSTR (soft reset: attributes, modes, scroll region), " q / SP q (ignored).
 *  SGR: 0 1 2 3 4 5 7 8 9 21(double-underline -> Underline) 22 23 24 25 27 28 29
 *       30-37 39 40-47 49 90-97 100-107, 38/48 with ;5;n (256) and ;2;r;g;b (truecolor),
 *       also the colon form 38:5:n / 38:2::r:g:b. Parameters default to 0 when empty.
 *  OSC: 0 / 2 ;title  -> screen->setTitle(); terminated by BEL or ST (ESC \). Others ignored.
 *
 * Options:
 *  - implicitCr (default true): LF/VT/FF also perform a carriage return. Required for MCU
 *    firmware that prints bare "\n". Harmless for Linux consoles that send "\r\n".
 *  - encoding: any name accepted by QStringDecoder (UTF-8, ISO-8859-1, GB18030 when ICU
 *    is available...). Invalid sequences decode to U+FFFD, never lose sync.
 *
 * Thread affinity: GUI thread (called from TerminalWidget::feedData).
 */
class AnsiParser : public QObject
{
    Q_OBJECT
public:
    explicit AnsiParser(TerminalScreen* screen, QObject* parent = nullptr);
    ~AnsiParser() override;

    TerminalScreen* screen() const;

    /// Decoder name; setting an unknown name falls back to "UTF-8" and returns false.
    bool setEncoding(const QString& name);
    QString encoding() const;
    /// Codec names offered in Preferences: "UTF-8", "ISO-8859-1", "GB18030" (if available), "System".
    static QStringList availableEncodings();

    bool implicitCr() const;
    void setImplicitCr(bool on);

    bool cursorKeyApplicationMode() const;   ///< DECCKM state, read by TerminalWidget for arrow keys
    bool bracketedPasteMode() const;

    /// Decode and interpret `data`. Multi-byte characters split across calls are handled
    /// by the stateful decoder; escape sequences split across calls by the state machine.
    void feed(const QByteArray& data);

    /// Reset the parser state machine and the decoder (not the screen).
    void reset();

signals:
    /// Bytes the terminal must transmit back to the device (DSR / DA replies).
    void responseRequested(const QByteArray& data);
    void cursorKeyModeChanged(bool applicationMode);
    void bracketedPasteChanged(bool on);

private:
    enum class State {
        Ground, Escape, EscapeIntermediate,
        CsiEntry, CsiParam, CsiIntermediate, CsiIgnore,
        OscString, DcsEntry, DcsParam, DcsIntermediate, DcsPassthrough, DcsIgnore,
        SosPmApcString
    };

    void processCodePoint(char32_t cp);
    void executeC0(char32_t cp);
    void dispatchEscape(char32_t final);
    void dispatchCsi(char32_t final);
    void dispatchOsc();
    void applySgr();
    void clearSequence();
    void collectParam(char32_t cp);
    int param(int index) const;                          ///< raw value at index (0 when missing)
    int paramOr(int index, int fallback) const;          ///< value at index, or `fallback` when missing or zero

    // Internal helpers (term-core implementation detail).
    QString decodeChunk(const QByteArray& data);         ///< stateful decode; UTF-8 chunk-boundary repair (see .cpp)
    void flushText();                                    ///< putText() the pending printable run
    void setMode(int mode, bool on, bool isPrivate);     ///< SM/RM and DECSET/DECRST for one parameter
    void softReset();                                    ///< DECSTR
    void alignmentPattern();                             ///< DECALN
    char32_t mapCharset(char32_t cp) const;              ///< GL mapping (DEC Special Graphics) of a printable
    void resetCharsets();                                ///< G0/G1 = ASCII, GL = G0, saved copies too
    void setCursorKeyApplicationMode(bool on);           ///< updates m_cursorKeyApp, emits on change
    void setBracketedPaste(bool on);                     ///< updates m_bracketedPaste, emits on change
    bool extendedColor(int index, Terminal::Color& out, int& consumed) const;   ///< SGR 38/48/58 sub-sequence
    QString describeSequence(const QString& prefix, char32_t final) const;      ///< for debug logging

    TerminalScreen* m_screen;
    State m_state = State::Ground;
    QStringDecoder m_decoder;
    QString m_encoding;
    QString m_pendingHighSurrogate;
    QByteArray m_pendingBytes;       ///< UTF-8 only: valid-so-far incomplete sequence carried into the next feed()
    bool m_utf8 = true;              ///< m_decoder is UTF-8: decodeChunk() handles chunk boundaries itself

    // current sequence being collected
    QVector<int> m_params;           ///< numeric parameters (sub-parameters after ':' folded per SGR rules)
    QVector<QVector<int>> m_subParams; ///< optional colon-separated sub-parameters per param index
    QString m_intermediates;         ///< collected intermediate bytes 0x20-0x2F
    bool m_privateMarker = false;    ///< '?' / '>' / '<' / '=' seen after CSI
    char32_t m_privateChar = 0;
    QString m_oscString;
    bool m_implicitCr = true;
    bool m_cursorKeyApp = false;
    bool m_bracketedPaste = false;
    bool m_lineFeedNewLine = false;  ///< LNM (mode 20): LF also performs CR
    bool m_inSubParam = false;       ///< digits currently go to the last sub-parameter
    bool m_paramOverflow = false;    ///< more than the supported number of parameters: ignore the rest
    qsizetype m_stringLength = 0;    ///< code points consumed by the current DCS/SOS/PM/APC string
    QString m_textRun;               ///< consecutive printable code points awaiting putText()

    // VT100 character sets: 'B' = US-ASCII, '0' = DEC Special Graphics (parser-side only, the
    // screen always stores Unicode). One DECSC slot, not duplicated per alternate screen.
    char m_charset[2] = {'B', 'B'};  ///< G0, G1 designations
    int m_glCharset = 0;             ///< 0 = G0 (SI/LS0), 1 = G1 (SO/LS1)
    char m_savedCharset[2] = {'B', 'B'};
    int m_savedGl = 0;
};
